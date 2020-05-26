/*
 * FTP Server for ESP8266/ESP32
 * based on FTP Serveur for Arduino Due and Ethernet shield (W5100) or WIZ820io (W5200)
 * based on Jean-Michel Gallego's work
 * modified to work with esp8266 SPIFFS by David Paiva david@nailbuster.com
 * modified to work with esp8266 LitteFS by Daniel Plasa dplasa@gmail.com
 * Also done some code reworks and all string contants are now in flash memory 
 * by using F(), PSTR() ... on the string literals.  
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "espFtpServer.h"

#ifdef ESP8266
#include <ESP8266WiFi.h>
#elif defined ESP32
#include <WiFi.h>
#endif

#include "espFtpCommands.h"

WiFiServer controlServer(FTP_CTRL_PORT);
WiFiServer dataServer(FTP_DATA_PORT_PASV);

// helper macros
#define FTP_STR(s) FTP_STR2(s)
#define FTP_STR2(s) #s
#define FTP_SEND_MSG(code, fmt, ...)                                     \
  do                                                                     \
  {                                                                      \
    FTP_DEBUG_MSG(">>> " FTP_STR(code) " " fmt, ##__VA_ARGS__);          \
    control.printf_P(PSTR(FTP_STR(code) " " fmt "\r\n"), ##__VA_ARGS__); \
  } while (0)

#define FTP_SEND_DASHMSG(code, fmt, ...)                                 \
  do                                                                     \
  {                                                                      \
    FTP_DEBUG_MSG(">>> " FTP_STR(code) "-" fmt, ##__VA_ARGS__);          \
    control.printf_P(PSTR(FTP_STR(code) "-" fmt "\r\n"), ##__VA_ARGS__); \
  } while (0)

// some constants
static const char aSpace[] PROGMEM = " ";
static const char aSlash[] PROGMEM = "/";

// constructor
FtpServer::FtpServer(FS &_FSImplementation) : THEFS(_FSImplementation) {}

void FtpServer::begin(const String &uname, const String &pword)
{
  _FTP_USER = uname;
  _FTP_PASS = pword;

  iniVariables();

  // Tells the ftp server to begin listening for incoming connections
  controlServer.begin();
  dataServer.begin();
}

void FtpServer::stop()
{
  abortTransfer();
  disconnectClient(false);
  controlServer.stop();
  dataServer.stop();
}

void FtpServer::iniVariables()
{
  // Default Data connection is Active
  dataPassiveConn = true;

  // Set the root directory
  cwd = FPSTR(aSlash);

  // init internal status vars
  cmdState = cInit;
  transferState = tIdle;
  rnFrom.clear();

  // reset control connection input buffer, clear previous command
  cmdLine.clear();
  cmdString.clear();
  parameters.clear();
  command = 0;

  // free any used fileBuffer
  freeBuffer();
}

void FtpServer::handleFTP()
{
  //
  // control connection state sequence is
  //  cInit
  //    |
  //    V
  //  cWait
  //    |
  //    V
  //  cCheck -----------+
  //    |               | (no username but password set)
  //    V               |
  //  cUserId ----------+---+
  //    |               |   |
  //    +<--------------+   |
  //    V                   | (no password set)
  //  cPassword             |
  //    |                   |
  //    +<------------------+
  //    V
  //  cLoginOk
  //    |
  //    V
  //  cProcess
  //

  // if ((int32_t)(millisDelay - millis()) > 0)
  //   return;

  if (cmdState == cInit)
  {
    if (control.connected())
    {
      abortTransfer();
      disconnectClient(false);
    }
    iniVariables();
    cmdState = cWait;
  }

  else if (cmdState == cWait) // FTP control server waiting for connection
  {
    if (controlServer.hasClient())
    {
      control = controlServer.available();

      // wait 10s for login command
      updateTimeout(10);
      cmdState = cCheck;
    }
  }

  else if (cmdState == cCheck) // FTP control server check/setup control connection
  {
    if (control.connected()) // A client connected, say "220 Hello client!"
    {
      FTP_DEBUG_MSG("control server got connection from %s:%d",
                    control.remoteIP().toString().c_str(), control.remotePort());

      FTP_SEND_MSG(220, "(espFTP " FTP_SERVER_VERSION ")");

      if (_FTP_USER.length())
      {
        // FTP_SEND_MSG(332, "Need account for login.");
        cmdState = cUserId;
      }
      else if (_FTP_PASS.length())
      {
        // FTP_SEND_MSG(331, "Please specify the password.");
        cmdState = cPassword;
      }
      else
      {
        cmdState = cLoginOk;
      }
    }
  }

  else if (cmdState == cLoginOk) // tell client "Login ok!"
  {
    FTP_SEND_MSG(230, "Login successful.");
    updateTimeout(sTimeOut);
    cmdState = cProcess;
  }

  //
  // all other command states need to process commands froms control connection
  //
  else if (readChar() > 0)
  {
    // enforce USER than PASS commands before anything else
    if (((cmdState == cUserId) && (FTP_CMD(USER) != command)) ||
        ((cmdState == cPassword) && (FTP_CMD(PASS) != command)))
    {
      FTP_SEND_MSG(530, "Please login with USER and PASS.");
      FTP_DEBUG_MSG("ignoring before login: cwd=%s cmd[%x]=%s, params='%s'", cwd.c_str(), command, cmdString.c_str(), parameters.c_str());
      command = 0;
      return;
    }

    // process the command
    int8_t rc = processCommand();
    // returns
    // -1 : command processing indicates, we have to close control (e.g. QUIT)
    //  0 : not yet finished, just call processCommend() again
    //  1 : finished
    if (rc < 0)
    {
      cmdState = cInit;
    }
    if (rc > 0)
    {
      // clear current command, so readChar() can fetch the next command
      command = 0;

      // command was successful, update command state
      if (cmdState == cUserId)
      {
        if (_FTP_PASS.length())
        {
          // wait 10s for PASS command
          updateTimeout(10);
          FTP_SEND_MSG(331, "Please specify the password.");
          cmdState = cPassword;
        }
        else
        {
          cmdState = cLoginOk;
        }
      }
      else if (cmdState == cPassword)
      {
        cmdState = cLoginOk;
      }
      else
      {
        updateTimeout(sTimeOut);
      }
    }
  }

  //
  // general connection handling
  // (if we have an established control connection)
  //
  if (cmdState >= cCheck)
  {
    // detect lost/closed by remote connections
    if (!control.connected() || !control)
    {
      cmdState = cInit;
      FTP_DEBUG_MSG("client lost or disconnected");
    }

    // check for timeout
    if (!((int32_t)(millisEndConnection - millis()) > 0))
    {
      FTP_SEND_MSG(530, "Timeout.");
      FTP_DEBUG_MSG("client connection timed out");
      cmdState = cInit;
    }

    // handle data file transfer
    if (transferState == tRetrieve) // Retrieve data
    {
      if (!doRetrieve())
      {
        closeTransfer();
        transferState = tIdle;
      }
    }
    else if (transferState == tStore) // Store data
    {
      if (!doStore())
      {
        closeTransfer();
        transferState = tIdle;
      }
    }
  }
}

void FtpServer::disconnectClient(bool gracious)
{
  FTP_DEBUG_MSG("Disconnecting client");
  abortTransfer();
  if (gracious)
  {
    FTP_SEND_MSG(221, "Goodbye.");
  }
  else
  {
    FTP_SEND_MSG(231, "Service terminated.");
  }
  control.stop();
}

int8_t FtpServer::processCommand()
{
  // assume successful operation by default
  int8_t rc = 1;

  // make the full path of parameters (even if this makes no sense for all commands)
  String path = getFileName(parameters, true);
  FTP_DEBUG_MSG("processing: cmd=%s[%x], params='%s' (cwd='%s')", cmdString.c_str(), command, parameters.c_str());

  ///////////////////////////////////////
  //                                   //
  //      ACCESS CONTROL COMMANDS      //
  //                                   //
  ///////////////////////////////////////

  //
  //  USER - Provide username
  //
  if (FTP_CMD(USER) == command)
  {
    if (_FTP_USER.length() && (_FTP_USER != parameters))
    {
      FTP_SEND_MSG(430, "User not found.");
      command = 0;
      rc = 0;
    }
    else
    {
      FTP_DEBUG_MSG("USER ok");
    }
  }

  //
  //  PASS - Provide password
  //
  else if (FTP_CMD(PASS) == command)
  {
    if (_FTP_PASS.length() && (_FTP_PASS != parameters))
    {
      FTP_SEND_MSG(430, "Password invalid.");
      command = 0;
      rc = 0;
    }
    else
    {
      FTP_DEBUG_MSG("PASS ok");
    }
  }

  //
  //  QUIT
  //
  else if (FTP_CMD(QUIT) == command)
  {
    disconnectClient();
    rc = -1;
  }

  //
  //  NOOP
  //
  else if (FTP_CMD(NOOP) == command)
  {
    FTP_SEND_MSG(200, "Zzz...");
  }

  //
  //  CDUP - Change to Parent Directory
  //
  else if (FTP_CMD(CDUP) == command)
  {
    // up one level
    cwd = getPathName("", false);
    FTP_SEND_MSG(250, "Directory successfully changed.");
  }

  //
  //  CWD - Change Working Directory
  //
  else if (FTP_CMD(CWD) == command)
  {
    if (parameters == F(".")) // 'CWD .' is the same as PWD command
    {
      command = FTP_CMD(PWD); // make CWD a PWD command ;-)
      rc = 0;                 // indicate we need another processCommand() call
    }
    else if (parameters == F("..")) // 'CWD ..' is the same as CDUP command
    {
      command = FTP_CMD(CDUP); // make CWD a CDUP command ;-)
      rc = 0;                  // indicate we need another processCommand() call
    }
    else
    {
#if (defined esp8266FTPServer_SPIFFS)
      // SPIFFS has no directories, it's always ok
      cwd = path;
      FTP_SEND_MSG(250, "Directory successfully changed.");
#else
      // check if directory exists
      file = THEFS.open(path, "r");
      if (file.isDirectory())
      {
        cwd = path;
        FTP_SEND_MSG(250, "Directory successfully changed.");
      }
      else
      {
        FTP_SEND_MSG(550, "Failed to change directory.");
      }
      file.close();
#endif
    }
  }

  //
  //  PWD - Print Directory
  //
  else if (FTP_CMD(PWD) == command)
  {
    FTP_SEND_MSG(257, "\"%s\" is the current directory.", cwd.c_str());
  }

  ///////////////////////////////////////
  //                                   //
  //    TRANSFER PARAMETER COMMANDS    //
  //                                   //
  ///////////////////////////////////////

  //
  //  MODE - Transfer Mode
  //
  else if (FTP_CMD(MODE) == command)
  {
    if (parameters == F("S"))
      FTP_SEND_MSG(504, "Only S(tream) mode is suported");
    else
      FTP_SEND_MSG(200, "Mode set to S.");
  }

  //
  //  PASV - Passive data connection management
  //
  else if (FTP_CMD(PASV) == command)
  {
    // stop a possible previous data connection
    data.stop();
    // tell client to open data connection to our ip:dataPort
    dataPort = FTP_DATA_PORT_PASV;
    dataPassiveConn = true;
    String ip = control.localIP().toString();
    ip.replace(".", ",");
    FTP_SEND_MSG(227, "Entering Passive Mode (%s,%d,%d).", ip.c_str(), dataPort >> 8, dataPort & 255);
  }

  //
  //  PORT - Data Port, Active data connection management
  //
  else if (FTP_CMD(PORT) == command)
  {
    if (data)
      data.stop();

    // parse IP and data port of "PORT ip,ip,ip,ip,port,port"
    uint8_t parsecount = 0;
    uint8_t tmp[6];
    const char *p = parameters.c_str();
    while (parsecount < sizeof(tmp))
    {
      tmp[parsecount++] = atoi(p);
      p = strchr(p, ',');
      if (NULL == p || *(++p) == '\0')
        break;
    }
    if (parsecount < sizeof(tmp))
    {
      FTP_SEND_MSG(501, "Can't interpret parameters");
    }
    else
    {
      // copy first 4 bytes = IP
      for (uint8_t i = 0; i < 4; ++i)
        dataIP[i] = tmp[i];
      // data port is 5,6
      dataPort = tmp[4] * 256 + tmp[5];
      FTP_SEND_MSG(200, "PORT command successful");
      dataPassiveConn = false;
      FTP_DEBUG_MSG("Data connection management Active, using %s:%u", dataIP.toString().c_str(), dataPort);
    }
  }

  //
  //  STRU - File Structure
  //
  else if (FTP_CMD(STRU) == command)
  {
    if (parameters == F("F"))
      FTP_SEND_MSG(504, "Only F(ile) is suported");
    else
      FTP_SEND_MSG(200, "Structure set to F.");
  }

  //
  //  TYPE - Data Type
  //
  else if (FTP_CMD(TYPE) == command)
  {
    if (parameters == F("A"))
      FTP_SEND_MSG(200, "TYPE is now ASII.");
    else if (parameters == F("I"))
      FTP_SEND_MSG(200, "TYPE is now 8-bit Binary.");
    else
      FTP_SEND_MSG(504, "Unrecognised TYPE.");
  }

  ///////////////////////////////////////
  //                                   //
  //        FTP SERVICE COMMANDS       //
  //                                   //
  ///////////////////////////////////////

  //
  //  ABOR - Abort
  //
  else if (FTP_CMD(ABOR) == command)
  {
    abortTransfer();
    FTP_SEND_MSG(226, "Data connection closed");
  }

  //
  //  DELE - Delete a File
  //
  else if (FTP_CMD(DELE) == command)
  {
    if (parameters.length() == 0)
      FTP_SEND_MSG(501, "No file name");
    else
    {
      if (!THEFS.exists(path))
      {
        FTP_SEND_MSG(550, "Delete operation failed, file '%s' not found.", path.c_str());
      }
      else if (THEFS.remove(path))
      {
        FTP_SEND_MSG(250, "Delete operation successful.");
      }
      else
      {
        FTP_SEND_MSG(450, "Delete operation failed.");
      }
    }
  }

  //
  //  LIST - List directory contents
  //  MLSD - Listing for Machine Processing (see RFC 3659)
  //  NLST - Name List
  //
  else if ((FTP_CMD(LIST) == command) || (FTP_CMD(MLSD) == command) || (FTP_CMD(NLST) == command))
  {
    rc = dataConnect(); // returns -1: no data connection, 0: need more time, 1: data ok
    if (rc < 0)
    {
      FTP_SEND_MSG(425, "No data connection");
      rc = 1; // mark command as processed
    }
    else if (rc > 0)
    {
      FTP_SEND_MSG(150, "Accepted data connection");
      uint16_t dirCount = 0;
      Dir dir = THEFS.openDir(path);
      while (dir.next())
      {
        ++dirCount;
        bool isDir = false;
        String fn = dir.fileName();
        if (cwd == FPSTR(aSlash) && fn[0] == '/')
          fn.remove(0, 1);
        isDir = dir.isDirectory();
        if (FTP_CMD(LIST) == command)
        {
          if (isDir)
          {
            data.printf_P(PSTR("+d\r\n,\t%s\r\n"), fn.c_str());
          }
          else
          {
            data.printf_P(PSTR("+r,s%lu\r\n,\t%s\r\n"), (uint32_t)dir.fileSize(), fn.c_str());
          }
        }
        else if (FTP_CMD(MLSD) == command)
        {
          // "modify=20170122163911;type=dir;UNIX.group=0;UNIX.mode=0775;UNIX.owner=0; dirname"
          // "modify=20170121000817;size=12;type=file;UNIX.group=0;UNIX.mode=0644;UNIX.owner=0; filename"
          file = dir.openFile("r");
          data.printf_P(PSTR("modify=%s;UNIX.group=0;UNIX.owner=0;UNIX.mode="), makeDateTimeStr(file.getLastWrite()).c_str());
          file.close();
          if (isDir)
          {
            data.printf_P(PSTR("0755;type=dir; "));
          }
          else
          {
            data.printf_P(PSTR("0644;size=%lu;type=file; "), (uint32_t)dir.fileSize());
          }
          data.printf_P(PSTR("%s\r\n"), fn.c_str());
        }
        else if (FTP_CMD(NLST) == command)
        {
          data.println(fn);
        }
        else
        {
          FTP_DEBUG_MSG("Implemetation of %s [%x] command - internal BUG", cmdString.c_str(), command);
        }
      }

      if (FTP_CMD(MLSD) == command)
      {
        control.println(F("226-options: -a -l\r\n"));
      }
      FTP_SEND_MSG(226, "%d matches total", dirCount);
    }
#if defined ESP32
    File root = THEFS.open(cwd);
    if (!root)
    {
      FTP_SEND_MSG(550, "Can't open directory " + cwd);
      // return;
    }
    else
    {
      // if(!root.isDirectory()){
      // 		FTP_DEBUG_MSG("Not a directory: '%s'", cwd.c_str());
      // 		return;
      // }

      File file = root.openNextFile();
      while (file)
      {
        if (file.isDirectory())
        {
          data.println("+r,s <DIR> " + String(file.name()));
          // Serial.print("  DIR : ");
          // Serial.println(file.name());
          // if(levels){
          // 	listDir(fs, file.name(), levels -1);
          // }
        }
        else
        {
          String fn, fs;
          fn = file.name();
          // fn.remove(0, 1);
          fs = String(file.size());
          data.println("+r,s" + fs);
          data.println(",\t" + fn);
          nm++;
        }
        file = root.openNextFile();
      }
      FTP_SEND_MSG(226, "%s matches total", nm);
    }
#endif
    data.stop();
  }

#if defined ESP32
  //
  //  FIXME MLSD ESP32
  //
  else if (!strcmp(command, "MLSD"))
  {
    File root = THEFS.open(cwd);
    // if(!root){
    // 		control.println( "550, "Can't open directory " + cwd );
    // 		// return;
    // } else {
    // if(!root.isDirectory()){
    // 		Serial.println("Not a directory");
    // 		return;
    // }

    File file = root.openNextFile();
    while (file)
    {
      // if(file.isDirectory()){
      // 	data.println( "+r,s <DIR> " + String(file.name()));
      // 	// Serial.print("  DIR : ");
      // 	// Serial.println(file.name());
      // 	// if(levels){
      // 	// 	listDir(fs, file.name(), levels -1);
      // 	// }
      // } else {
      String fn, fs;
      fn = file.name();
      fn.remove(0, 1);
      fs = String(file.size());
      data.println("Type=file;Size=" + fs + ";" + "modify=20000101160656;" + " " + fn);
      nm++;
      // }
      file = root.openNextFile();
    }
    FTP_SEND_MSG(226, "-options: -a -l");
    FTP_SEND_MSG(226, "%d matches total", nm);
    // }
    data.stop();
  }
  //
  // NLST
  //
  else if (!strcmp(command, "NLST"))
  {
    File root = THEFS.open(cwd);
    if (!root)
    {
      FTP_SEND_MSG(550, "Can't open directory %s\n"), cwd.c_str());
    }
    else
    {
      File file = root.openNextFile();
      while (file)
      {
        data.println(file.name());
        nm++;
        file = root.openNextFile();
      }
      FTP_SEND_MSG(226, "%d matches total", nm);
    }
    data.stop();
  }
#endif

  //
  //  RETR - Retrieve
  //
  else if (FTP_CMD(RETR) == command)
  {
    if (parameters.length() == 0)
    {
      FTP_SEND_MSG(501, "No file name");
    }
    else
    {
      // open the file if not opened before (when re-running processCommand() since data connetion needs time)
      if (!file)
        file = THEFS.open(path, "r");
      if (!file)
      {
        FTP_SEND_MSG(550, "File '%s' not found.", parameters.c_str());
      }
      else if (!file.isFile())
      {
        FTP_SEND_MSG(450, "Cannot open file \"%s\".", parameters.c_str());
      }
      else
      {
        rc = dataConnect(); // returns -1: no data connection, 0: need more time, 1: data ok
        if (rc < 0)
        {
          FTP_SEND_MSG(425, "No data connection");
          rc = 1; // mark command as processed
        }
        else if (rc > 0)
        {
          transferState = tRetrieve;
          millisBeginTrans = millis();
          bytesTransfered = 0;
          uint32_t fs = file.size();
          if (allocateBuffer(fs > 32768 ? 32768 : fs))
          {
            FTP_DEBUG_MSG("Sending file '%s'", path.c_str());
            FTP_SEND_MSG(150, "%lu bytes to download", fs);
          }
          else
          {
            closeTransfer();
            FTP_SEND_MSG(451, "Internal error. Not enough memory.");
          }
        }
      }
    }
  }

  //
  //  STOR - Store
  //
  else if (FTP_CMD(STOR) == command)
  {
    if (parameters.length() == 0)
    {
      FTP_SEND_MSG(501, "No file name.");
    }
    else
    {
      FTP_DEBUG_MSG("STOR '%s'", path.c_str());
      if (!file)
        file = THEFS.open(path, "w");
      if (!file)
      {
        FTP_SEND_MSG(451, "Cannot open/create \"%s\"", path.c_str());
      }
      else
      {
        rc = dataConnect(); // returns -1: no data connection, 0: need more time, 1: data ok
        if (rc < 0)
        {
          FTP_SEND_MSG(425, "No data connection");
          file.close();
          rc = 1; // mark command as processed
        }
        else if (rc > 0)
        {
          transferState = tStore;
          millisBeginTrans = millis();
          bytesTransfered = 0;
          if (allocateBuffer(2048))
          {
            FTP_DEBUG_MSG("Receiving file '%s' => %s", parameters.c_str(), path.c_str());
            FTP_SEND_MSG(150, "Connected to port %d", dataPort);
          }
          else
          {
            closeTransfer();
            FTP_SEND_MSG(451, "Internal error. Not enough memory.");
          }
        }
      }
    }
  }

  //
  //  MKD - Make Directory
  //
  else if (FTP_CMD(MKD) == command)
  {
#if (defined esp8266FTPServer_SPIFFS)
    FTP_SEND_MSG(550, "Create directory operation failed."); //not support on SPIFFS
#else
    FTP_DEBUG_MSG("mkdir(%s)", path.c_str());
    if (THEFS.mkdir(path))
    {
      FTP_SEND_MSG(257, "\"%s\" created.", path.c_str());
    }
    else
    {
      FTP_SEND_MSG(550, "Create directory operation failed.");
    }
#endif
  }

  //
  //  RMD - Remove a Directory
  //
  else if (FTP_CMD(RMD) == command)
  {
#if (defined esp8266FTPServer_SPIFFS)
    FTP_SEND_MSG(550, "Remove directory operation failed."); //not support on SPIFFS
#else
    // check directory for files
    Dir dir = THEFS.openDir(path);
    if (dir.next())
    {
      //only delete if dir is empty!
      FTP_SEND_MSG(550, "Remove directory operation failed, directory is not empty.");
    }
    else
    {
      THEFS.rmdir(path);
      FTP_SEND_MSG(250, "Remove directory operation successful.");
    }
#endif
  }
  //
  //  RNFR - Rename From
  //
  else if (FTP_CMD(RNFR) == command)
  {
    if (parameters.length() == 0)
      FTP_SEND_MSG(501, "No file name");
    else
    {
      if (!THEFS.exists(path))
        FTP_SEND_MSG(550, "File \"%s\" not found.", path.c_str());
      else
      {
        FTP_SEND_MSG(350, "RNFR accepted - file \"%s\" exists, ready for destination", path.c_str());
        rnFrom = path;
      }
    }
  }
  //
  //  RNTO - Rename To
  //
  else if (FTP_CMD(RNTO) == command)
  {
    if (rnFrom.length() == 0)
      FTP_SEND_MSG(503, "Need RNFR before RNTO");
    else if (parameters.length() == 0)
      FTP_SEND_MSG(501, "No file name");
    else if (THEFS.exists(path))
      FTP_SEND_MSG(553, "\"%s\" already exists.", parameters.c_str());
    else
    {
      FTP_DEBUG_MSG("Renaming '%s' to '%s'", rnFrom.c_str(), path.c_str());
      if (THEFS.rename(rnFrom, path))
        FTP_SEND_MSG(250, "File successfully renamed or moved");
      else
        FTP_SEND_MSG(451, "Rename/move failure.");
    }
    rnFrom.clear();
  }

  ///////////////////////////////////////
  //                                   //
  //   EXTENSIONS COMMANDS (RFC 3659)  //
  //                                   //
  ///////////////////////////////////////

  //
  //  FEAT - New Features
  //
  else if (FTP_CMD(FEAT) == command)
  {
    FTP_SEND_DASHMSG(211, "Features:\r\n  MLSD\r\n  MDTM\r\n  SIZE\r\n211 End.");
  }

  //
  //  MDTM - File Modification Time (see RFC 3659)
  //
  else if (FTP_CMD(MDTM) == command)
  {
    file = THEFS.open(path, "r");
    if ((!file) || (0 == parameters.length()))
    {
      FTP_SEND_MSG(550, "Unable to retrieve time");
    }
    else
    {
      FTP_SEND_MSG(213, "%s", makeDateTimeStr(file.getLastWrite()).c_str());
    }
    file.close();
  }

  //
  //  SIZE - Size of the file
  //
  else if (FTP_CMD(SIZE) == command)
  {
    file = THEFS.open(path, "r");
    if ((!file) || (0 == parameters.length()))
    {
      FTP_SEND_MSG(450, "Cannot open file.");
    }
    else
    {
      FTP_SEND_MSG(213, "%lu", (uint32_t)file.size());
    }
    file.close();
  }

  //
  //  SITE - System command
  //
  else if (FTP_CMD(SITE) == command)
  {
    FTP_SEND_MSG(502, "SITE command not implemented");
  }

  //
  //  SYST - System information
  //
  else if (FTP_CMD(SYST) == command)
  {
    FTP_SEND_MSG(215, "UNIX Type: L8");
  }

  //
  //  Unrecognized commands ...
  //
  else
  {
    FTP_DEBUG_MSG("Unknown command: %s [%#x], param: '%s')", cmdString.c_str(), command, parameters.c_str());
    FTP_SEND_MSG(500, "unknown command \"%s\"", cmdString.c_str());
  }

  return rc;
}

int8_t FtpServer::dataConnect()
{
  int8_t rc = 1; // assume success

  if (!dataPassiveConn)
  {
    // active mode
    // open our own data connection
    data.stop();
    FTP_DEBUG_MSG("Open active data connection to %s:%u", dataIP.toString().c_str(), dataPort);
    data.connect(dataIP, dataPort);
    if (!data.connected())
      rc = -1;
  }
  else
  {
    // passive mode
    // wait for data connection from the client
    if (!data.connected())
    {
      if (dataServer.hasClient())
      {
        data.stop();
        data = dataServer.available();
        FTP_DEBUG_MSG("Got incoming (passive) data connection from %s:%u", data.remoteIP().toString().c_str(), data.remotePort());
      }
      else
      {
        // give me more time waiting for a data connection
        rc = 0;
      }
    }
  }
  return rc;
}

uint16_t FtpServer::allocateBuffer(uint16_t desiredBytes)
{
  // allocate a big buffer for file transfers
  uint16_t maxBlock = ESP.getMaxFreeBlockSize() / 2;

  if (desiredBytes > maxBlock)
    desiredBytes = maxBlock;

  while (fileBuffer == NULL && desiredBytes > 0)
  {
    fileBuffer = (uint8_t *)malloc(desiredBytes);
    if (NULL == fileBuffer)
    {
      FTP_DEBUG_MSG("Cannot allocate buffer for file transfer, re-trying");
      // try with less bytes
      desiredBytes--;
    }
    else
    {
      fileBufferSize = desiredBytes;
    }
  }
  return fileBufferSize;
}

void FtpServer::freeBuffer()
{
  free(fileBuffer);
  fileBuffer = NULL;
}

bool FtpServer::doRetrieve()
{
  // data connection lost or no more bytes to transfer?
  if (!data.connected() || (bytesTransfered >= file.size()))
  {
    return false;
  }

  // how many bytes to transfer left?
  uint32_t nb = (file.size() - bytesTransfered);
  if (nb > fileBufferSize)
    nb = fileBufferSize;

  // transfer the file
  FTP_DEBUG_MSG("Transfer %d bytes fs->client", nb);
  nb = file.readBytes((char *)fileBuffer, nb);
  if (nb > 0)
  {
    data.write(fileBuffer, nb);
    bytesTransfered += nb;
  }

  return (nb > 0);
}

bool FtpServer::doStore()
{
  // Avoid blocking by never reading more bytes than are available
  int16_t navail = data.available();

  if (navail > 0)
  {
    if (navail > fileBufferSize)
      navail = fileBufferSize;
    FTP_DEBUG_MSG("Transfer %d bytes client->fs", navail);
    navail = data.read(fileBuffer, navail);
    file.write(fileBuffer, navail);
  }

  if (!data.connected() && (navail <= 0))
  {
    // connection closed or no more bytes to read
    return false;
  }
  else
  {
    // inidcate, we need to be called again
    return true;
  }
}

void FtpServer::closeTransfer()
{
  uint32_t deltaT = (int32_t)(millis() - millisBeginTrans);
  if (deltaT > 0 && bytesTransfered > 0)
  {
    FTP_SEND_MSG(226, "File successfully transferred, %lu ms, %f kB/s.", deltaT, float(bytesTransfered) / deltaT);
  }
  else
    FTP_SEND_MSG(226, "File successfully transferred");

  freeBuffer();
  file.close();
  data.stop();
}

void FtpServer::abortTransfer()
{
  if (transferState > tIdle)
  {
    file.close();
    data.stop();
    FTP_SEND_MSG(426, "Transfer aborted");
  }
  freeBuffer();
  transferState = tIdle;
}

// Read a char from client connected to ftp server
//
//  returns:
//    -1 if cmdLine too long
//     0 cmdLine still incomplete (no \r or \n received yet)
//     1 cmdLine processed, command and parameters available

int8_t FtpServer::readChar()
{
  // only read/parse, if the previous command has been fully processed!
  if (command)
    return 1;

  while (control.available())
  {
    char c = control.read();
    // FTP_DEBUG_MSG("readChar() cmdLine='%s' <= %c", cmdLine.c_str(), c);

    // substitute '\' with '/'
    if (c == '\\')
      c = '/';

    // nl detected? then process line
    if (c == '\n' || c == '\r')
    {
      cmdLine.trim();

      // but only if we got at least chars in the line!
      if (0 == cmdLine.length())
        break;

      // search for space between command and parameters
      int pos = cmdLine.indexOf(FPSTR(aSpace));
      if (pos > 0)
      {
        parameters = cmdLine.substring(pos + 1);
        parameters.trim();
        cmdLine.remove(pos);
      }
      else
      {
        parameters.remove(0);
      }
      cmdString = cmdLine;

      // convert command to upper case
      cmdString.toUpperCase();

      // convert the (up to 4 command chars to numerical value)
      command = *(const uint32_t *)cmdString.c_str();

      // clear cmdline
      cmdLine.clear();
      FTP_DEBUG_MSG("readChar() success, command=%x, cmdString='%s', params='%s'", command, cmdString.c_str(), parameters.c_str());
      return 1;
    }
    else
    {
      // just add char
      cmdLine += c;
      if (cmdLine.length() > FTP_CMD_SIZE)
      {
        cmdLine.clear();
        FTP_SEND_MSG(500, "Line too long");
      }
    }
  }
  return 0;
}

// Get the complete path from cwd + parameters or complete filename from cwd + parameters
//
// 3 possible cases: parameters can be absolute path, relative path or only the name
//
// returns:
//    path WITHOUT file-/dirname (fullname=false)
//    full path WITH file-/dirname (fullname=true)
String FtpServer::getPathName(const String &param, bool fullname)
{
  String tmp;

  // is param an absoulte path?
  if (param[0] == '/')
  {
    tmp = param;
  }
  else
  {
    // start with cwd
    tmp = cwd;

    // if param != "" then add param
    if (param.length())
    {
      if (!tmp.endsWith(FPSTR(aSlash)))
        tmp += '/';
      tmp += param;
    }
    // => tmp becomes cdw [ + '/' + param ]
  }

  // strip filename
  if (!fullname)
  {
    // search rightmost '/'
    int lastslash = tmp.lastIndexOf(FPSTR(aSlash));
    if (lastslash >= 0)
    {
      tmp.remove(lastslash);
    }
  }
  // sanetize:
  // "" -> "/"
  // "/some/path/" => "/some/path"
  while (tmp.length() > 1 && tmp.endsWith(FPSTR(aSlash)))
    tmp.remove(cwd.length() - 1);
  if (tmp.length() == 0)
    tmp += '/';
  return tmp;
}

// Get the [complete] file name from cwd + parameters
//
// 3 possible cases: parameters can be absolute path, relative path or only the filename
//
// returns:
//    filename or filename with complete path
String FtpServer::getFileName(const String &param, bool fullFilePath)
{
  // build the filename with full path
  String tmp = getPathName(param, true);

  if (!fullFilePath)
  {
    // strip path
    // search rightmost '/'
    int lastslash = tmp.lastIndexOf(FPSTR(aSlash));
    if (lastslash > 0)
    {
      tmp.remove(0, lastslash);
    }
  }

  return tmp;
}

// Formats YYYYMMDDHHMMSS from a time_t timestamp
//
// uses the buf of the FtpServer to store the date string
//
// parameters:
//    timestamp
//
// return:
//    pointer to buf[0]

String FtpServer::makeDateTimeStr(time_t ft)
{
  struct tm *_tm = gmtime(&ft);
  String tmp;
  tmp.reserve(17);
  strftime((char *)tmp.c_str(), 17, "%Y%m%d%H%M%S", _tm);
  return tmp;
}

void FtpServer::updateTimeout(uint16_t s)
{
  millisEndConnection = s;
  millisEndConnection *= 60000UL;
  millisEndConnection += millis();
}
