#include <stdlib.h>
#include <stdio.h>
#include <windows.h>

//////////////////////////////////////////
// $PSMCFG,<A>,<B>,<C>,<D>,<E>,<F>,<G>*HH
//////////////////////////////////////////

#define ASCII_XON       0x11
#define ASCII_XOFF      0x13

#define SLC_SIGNATURE   "SLC"

enum DataType
{
    GPS_FROM_PORT_OFFSET  = 0,
    GPS_FROM_STERN_OFFSET,
    LAMP_FROM_PORT_OFFSET,
    LAMP_FROM_BOW_OFFSET,
    LAMP_HEIGHT,
    LENGTH_OVERALL,
    BREADTH,
    BEARING_CORRECTION,

    MAX
};

union Data
{
    struct
    {
        int gpsPortOffset, gpsSternOffset, lampPortOffset, lampBowOffset, lampHeight, lengthOverall, breadth, brgCorrection;
    };

    int values [MAX];
};

extern "C" void *__enclave_config = 0;

void showHelp ()
{
    printf ("USAGE:\n\n\t-P:port\n\t-B:baud rate\n");
}

bool parseArgs (const int argCount, char *args [], int& port, int& baud)
{
    for (int i = 1; i < argCount; ++ i)
    {
        const char *arg = args [i];

        if (arg [0] != '-' && arg [0] != '/')
            return false;

        switch (toupper (arg [1]))
        {
            case 'B':
                if (arg [2] == ':')
                    baud = atoi (arg + 3);
                else
                    return false;

                break;

            case 'P':
                if (arg [2] == ':')
                    port = atoi (arg + 3);
                else
                    return false;

                break;

            case 'H':
            default:
                showHelp (); return false;
        }
    }

    return true;
}

void configurePort (HANDLE portHandle, const int baud)
{
    COMMTIMEOUTS timeouts;
    DCB          state;

    memset (& state, 0, sizeof (state));

    state.DCBlength = sizeof (state);

    SetupComm (portHandle, 4096, 4096); 
    PurgeComm (portHandle, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR); 
    GetCommState (portHandle, & state);
    GetCommTimeouts (portHandle, & timeouts);

    timeouts.ReadIntervalTimeout        = 1000;
    timeouts.ReadTotalTimeoutMultiplier = 1;
    timeouts.ReadTotalTimeoutConstant   = 3000;

    state.fInX     = 
    state.fOutX    =
    state.fParity  =
    state.fBinary  = 1;
    state.BaudRate = baud;
    state.ByteSize = 8;
    state.XoffChar = ASCII_XOFF;
    state.XonChar  = ASCII_XON;
    state.XonLim   =
    state.XoffLim  = 100;
    state.Parity = PARITY_NONE;
    state.StopBits = ONESTOPBIT;

    SetCommTimeouts (portHandle, & timeouts);
    SetCommState (portHandle, & state);
}

void buildSendSentence (HANDLE portHandle, const char *body)
{
    char          sentence [100];
    unsigned char crc;
    int           i;
    unsigned long bytesWritten;

    strcpy (sentence, body);
    strcat (sentence, "*00\n");

    for (i = 2, crc = sentence [1]; sentence [i] && sentence [i] != '*'; crc ^= sentence [i++]);

    if (sentence [i] == '*')
        sprintf (sentence + (i + 1), "%02X\n", crc);

    WriteFile (portHandle, sentence, strlen (sentence), & bytesWritten, 0);
}

void sendRequest (HANDLE portHandle)
{
    buildSendSentence (portHandle, "$PSMCFG,,,,,,,,,");
}

bool isIncomingDataAvailable (HANDLE portHandle)
{
    COMSTAT       commState;
    unsigned long errorFlags;

    ClearCommError (portHandle, & errorFlags, & commState);

    return commState.cbInQue > 6;
}

void waitForResponse (HANDLE portHandle, char *buffer, const int size)
{
    char curByte;
    int  count = 0;

    memset (buffer, 0, size);

    do
    {
        unsigned long bytesRead;

        if (ReadFile (portHandle, & curByte, 1, & bytesRead, 0) && bytesRead > 0)
        {
            if (curByte == '$')
            {
                memset (buffer, 0, sizeof (buffer));

                count = 0;

            }
            else if ((curByte == '\r' || curByte == '\n') && count < 2)
            {
                curByte = 0;
            }
            else
            {
                buffer [count++] = curByte;
            }
        }
        else
        {
            curByte = 0;
        }
    }
    while (curByte != '\r' && curByte !='\n' && strchr (buffer, '$') == 0);
}

void parse (char *dataString, char *fields [], int& fieldCount)
{
    fieldCount = 1;

    fields [0] = dataString;

    for (char *curChar = dataString; *curChar; ++ curChar)
    {
        if (*curChar == '*')
        {
            *curChar = '\0';
        }
        else if (*curChar == ',')
        {
            *curChar = '\0';

            fields [fieldCount++] = curChar + 1;
        }
    }
}

char getCommand ()
{
    char command;
    bool acceptableChar = true;
    bool crlf = false;

    do
    {
        if (!crlf)
            printf ("\nCommand: ");

        command = toupper (getchar ());

        acceptableChar = strchr ("BRDQ12345678", command);
        crlf           = command == '\r' || command == '\n';

        if (!acceptableChar && !crlf)
            printf ("Invalid command\n");
    }
    while (!acceptableChar);

    return command;
}

void changeFieldValue (HANDLE portHandle, const int fieldIndex, Data& data, char *valueNames [])
{
    char sentence [100], valueString [50];
    int  newValue = 0;
    
    printf ("%s [%d]: ", valueNames [fieldIndex], data.values [fieldIndex]);
    scanf ("%s", valueString);

    if (!*valueString || !isdigit (*valueString))
    {
        printf ("Not a number - ignored\n"); return;
    }

    if (fieldIndex == DataType::BEARING_CORRECTION)
        newValue = (int) (atof (valueString) * 10.0 + 0.1);
    else
        newValue = atoi (valueString);

    strcpy (sentence, "$PSMCFG");

    for (int i = 0; i < MAX; ++ i)
    {
        strcat (sentence, ",");

        if (i == fieldIndex)
        {
            char field [20];

            sprintf (field, "%d", newValue);
            strcat (sentence, field);
        }
    }

    buildSendSentence (portHandle, sentence);
    Sleep (300);
}

void showStatus (Data& data, char *valueNames [])
{
    printf ("1 - %s\t[%d]\n"
            "2 - %s\t[%d]\n"
            "3 - %s\t[%d]\n"
            "4 - %s\t[%d]\n"
            "5 - %s\t\t\t[%d]\n"
            "6 - %s\t\t[%d]\n"
            "7 - %s\t\t[%d]\n"
            "8 - %s\t\t[%.1f]\n"
            "D - Display status\n"
            "B - Backup\n"
            "R - Restore\n"
            "Q - Quit\n\n",
            valueNames [0], data.gpsPortOffset, valueNames [1], data.gpsSternOffset, valueNames [2], data.lampPortOffset,
            valueNames [3], data.lampBowOffset, valueNames [4], data.lampHeight, valueNames [5],
            data.lengthOverall, valueNames [6], data.breadth, valueNames [7], (double) data.brgCorrection * 0.1);
}

bool getBackupFileName (char *filePath)
{
    printf ("Enter backup file name [%s]: ", filePath);
    scanf ("%s", filePath);

    return strlen (filePath) > 0;
}

void backup (Data& data)
{
    char filePath [100] = { "slconfig.dat" };

    if (getBackupFileName (filePath))
    {
        HANDLE backupFile = CreateFile (filePath, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);

        if (backupFile != INVALID_HANDLE_VALUE)
        {
            unsigned long  bytesWritten;
            unsigned short size = sizeof (data);

            WriteFile (backupFile, SLC_SIGNATURE, 3, & bytesWritten, 0);
            WriteFile (backupFile, & size, sizeof (size), & bytesWritten, 0);
            WriteFile (backupFile, & data, sizeof (data), & bytesWritten, 0);
            CloseHandle (backupFile);

            printf ("Configuration has been backed up to %s\n\n", filePath);
        }
        else
        {
            printf ("Unable to write to file '%s', error code %d\n\n", filePath, GetLastError ());
        }
    }
}

void restore (HANDLE portHandle, Data& data)
{
    char filePath [100] = { "slconfig.dat" };

    if (getBackupFileName (filePath))
    {
        HANDLE backupFile = CreateFile (filePath, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);

        if (backupFile != INVALID_HANDLE_VALUE)
        {
            unsigned long  bytesRead;
            unsigned short size;
            char           signature [3], sentence [100], field [20], input;

            ReadFile (backupFile, signature, sizeof (signature), & bytesRead, 0);

            if (memcmp (signature, SLC_SIGNATURE, 3) != 0)
            {
                printf ("It is not SearchLight backup file.\n\n"); return;
            }

            ReadFile (backupFile, & size, sizeof (size), & bytesRead, 0);

            printf ("Reading %d bytes...\n", size);
            ReadFile (backupFile, & data, size, & bytesRead, 0);
            CloseHandle (backupFile);

            printf ("Confirm to upload (y/n) ");

            do
            {
                scanf ("%c", & input);

                input = tolower (input);

                if (input != '\n' && input != '\r')
                    printf ("Confirm to upload (y/n) ");
            }
            while (input != 'y' && input != 'n');

            printf ("\n");

            if (input == 'y')
            {
                printf ("Uploading...\n");

                strcpy (sentence, "$PSMCFG");

                for (int i = 0; i < MAX; ++ i)
                {
                    strcat (sentence, ",");
                    sprintf (field, "%d", data.values [i]);
                    strcat (sentence, field);
                }

                buildSendSentence (portHandle, sentence);
                Sleep (300);
                printf ("Configuration has been restored from %s\n\n", filePath);
            }
        }
        else
        {
            printf ("Unable to read from file '%s', error code %d\n\n", filePath, GetLastError ());
        }
    }
}

int main (int argCount, char *args [])
{
    HANDLE portHandle;
    int    port = 1;
    int    baud = 4800;
    Data   data;
    char  *valueNames  [] = { "GPS offset from port side", "GPS offset from stern", "Lamp offset from port side", "Lamp offset from bow",
                              "Lamp height", "Ship length overall", "Ship breadth", "Bearing correction" };

    printf ("SeaMaster SearcLight configuration tool\n");

    if (parseArgs (argCount, args, port, baud))
    {
        char portName [50];
        char buffer [100];

        sprintf (portName, "\\\\.\\COM%d", port);

        portHandle = CreateFileA (portName, GENERIC_READ | GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);

        if (portHandle != INVALID_HANDLE_VALUE)
        {
            char *fields [50], command;
            int   fieldCount;

            printf ("COM%d opened\n", port);

            configurePort (portHandle, baud);

            do
            {
                sendRequest (portHandle);
                Sleep (100);

                do
                {
                    waitForResponse (portHandle, buffer, sizeof (buffer));

                    fieldCount = 0;

                    parse (buffer, fields, fieldCount);

                    for (int i = 0; i < MAX; ++ i)
                        data.values [i] = atoi  (fields [i+1]);

                    Sleep (100);
                }
                while (isIncomingDataAvailable (portHandle));

                showStatus (data, valueNames);

                command = getCommand ();

                if (command >= '1' && command <= '8')
                    changeFieldValue (portHandle, command - '1', data, valueNames);
                else if (command == 'B')
                    backup (data);
                else if (command == 'R')
                    restore (portHandle, data);
            }
            while (command != 'Q');

            CloseHandle (portHandle);
        }
        else
        {
            printf ("Unable to open COM%d\n", port);
        }
    }

    exit (0);
}