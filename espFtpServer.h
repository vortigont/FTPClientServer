/*
 * FTP SERVER FOR ESP8266/ESP32
 * based on FTP Serveur for Arduino Due and Ethernet shield (W5100) or WIZ820io (W5200)
 * based on Jean-Michel Gallego's work
 * modified to work with esp8266 SPIFFS by David Paiva (david@nailbuster.com)
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

#ifndef ESP_FTP_SERVER_H
#define ESP_FTP_SERVER_H

#include <WiFiClient.h>
#include <FS.h>

/*******************************************************************************
 **                                                                            **
 **                       DEFINITIONS FOR FTP SERVER                           **
 **                                                                            **
 *******************************************************************************/

//
// DEBUG via Serial Console
//  Please select in your Arduino IDE menu Tools->Debug Port to enable debugging.
//  (This will provide DEBUG_ESP_PORT at compile time.)
//

// Use ESP8266 Core Debug functionality
#ifdef DEBUG_ESP_PORT
#define FTP_DEBUG_MSG(fmt, ...)                                      \
  do                                                                 \
  {                                                                  \
    DEBUG_ESP_PORT.printf_P(PSTR("[FTP] " fmt "\n"), ##__VA_ARGS__); \
    yield();                                                         \
  } while (0)
#else
#define FTP_DEBUG_MSG(...)
#endif

#define FTP_SERVER_VERSION "0.9.2-20200526"

#define FTP_CTRL_PORT 21         // Command port on wich server is listening
#define FTP_DATA_PORT_PASV 50009 // Data port in passive mode
#define FTP_TIME_OUT 5           // Disconnect client after 5 minutes of inactivity
#define FTP_CMD_SIZE 127         // allow max. 127 chars in a received command

class FtpServer
{
public:
  // contruct an instance of the FTP server using a
  // given FS object, e.g. SPIFFS or LittleFS
  FtpServer(FS &_FSImplementation);

  // starts the FTP server with username and password,
  // either one can be empty to enable anonymous ftp
  void begin(const String &uname, const String &pword);

  // stops the FTP server
  void stop();

  // set the FTP server's timeout in seconds
  void setTimeout(uint16_t timeout = FTP_TIME_OUT * 60);

  // needs to be called frequently (e.g. in loop() )
  // to process ftp requests
  void handleFTP();

private:
  FS &THEFS;

  enum internalState
  {
    cInit = 0,
    cWait,
    cCheck,
    cUserId,
    cPassword,
    cLoginOk,
    cProcess,

    tIdle,
    tRetrieve,
    tStore
  };
  void iniVariables();
  void disconnectClient(bool gracious = true);
  int8_t processCommand();
  int8_t dataConnect();

  bool doRetrieve();
  bool doStore();
  void closeTransfer();
  void abortTransfer();
  int32_t allocateBuffer(int32_t desiredBytes);
  void freeBuffer();

  String getPathName(const String& param, bool includeLast = false);
  String getFileName(const String &param, bool fullFilePath = false);
  String makeDateTimeStr(time_t fileTime);
  int8_t readChar();
  void updateTimeout(uint16_t timeout);

  WiFiClient control;
  WiFiClient data;

  File file;

  bool dataPassiveConn = true; // PASV (passive) mode is our default
  IPAddress dataIP;            // IP address for PORT (active) mode
  uint16_t dataPort =          // holds our PASV port number or the port number provided by PORT
      FTP_DATA_PORT_PASV;

  uint32_t command;       // numeric command code of command sent by the client
  String cmdLine;         // command line as read from client
  String cmdString;       // command as textual representation
  String parameters;      // parameters sent by client
  String cwd;             // the current directory
  String rnFrom;          // previous command was RNFR, this is the source file name

  internalState cmdState, // state of ftp control connection
      transferState;      // state of ftp data connection
  uint16_t sTimeOut =     // disconnect after 5 min of inactivity
      FTP_TIME_OUT * 60;
  uint32_t millisEndConnection, //
      millisBeginTrans,         // store time of beginning of a transaction
      bytesTransfered;          //
  uint8_t *fileBuffer = NULL;   // pointer to buffer for file transfer (by allocateBuffer)
  int32_t fileBufferSize;       // size of buffer
  String _FTP_USER;             // usename
  String _FTP_PASS;             // password
};

#endif // ESP_FTP_SERVER_H
