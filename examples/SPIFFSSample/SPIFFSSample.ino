/*
   This is an example sketch to show the use of the espFTP server.

   Please replace
     YOUR_SSID and YOUR_PASS
   with your WiFi's values and compile.

   If you want to see debugging output of the FTP server, please
   select select an Serial Port in the Arduino IDE menu Tools->Debug Port

   Send L via Serial Monitor, to display the contents of the FS
   Send F via Serial Monitor, to fromat the FS

   This example is provided as Public Domain
   Daniel Plasa <dplasa@gmail.com>

*/
#ifdef ESP8266
#include <FS.h>
#include <ESP8266WiFi.h>
#elif defined ESP32
#include <WiFi.h>
#include <SPIFFS.h>
#endif

#include <espFtpServer.h>

const char *ssid PROGMEM = "";
const char *password PROGMEM = "";

// Since SPIFFS is becoming deprecated but might still be in
// use in your Projects, tell the FtpServer to use SPIFFS
FtpServer ftpSrv(SPIFFS);

void setup(void)
{
  Serial.begin(74880);
  WiFi.begin(ssid, password);

  bool fsok = SPIFFS.begin();
  Serial.printf_P(PSTR("FS init: %S\n"), fsok ? PSTR("ok") : PSTR("fail!"));

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.printf_P(PSTR("."));
  }
  Serial.printf_P(PSTR("\nConnected to %S, IP address is %s\n"), ssid, WiFi.localIP().toString().c_str());

  // setup the ftp server with username and password
  // ports are defined in esFTP.h, default is
  //   21 for the control connection
  //   50009 for the data connection (passive mode by default)
  ftpSrv.begin(F("ftp"), F("ftp"));
}

enum consoleaction
{
  show,
  wait,
  format,
  list
};

consoleaction action = show;

void loop(void)
{
  // this is all you need
  // make sure to call handleFTP() frequently
  ftpSrv.handleFTP();

  //
  // Code below just to debug in Serial Monitor
  //
  if (action == show)
  {
    Serial.printf_P(PSTR("Enter 'F' to format, 'L' to list the contents of the FS\n"));
    action = wait;
  }
  else if (action == wait)
  {
    if (Serial.available())
    {
      char c = Serial.read();
      if (c == 'F')
        action = format;
      else if (c == 'L')
        action = list;
      else if (!(c == '\n' || c == '\r'))
        action = show;
    }
  }
  else if (action == format)
  {
    uint32_t startTime = millis();
    SPIFFS.format();
    Serial.printf_P(PSTR("FS format done, took %lu ms!\n"), millis() - startTime);
    action = show;
  }
  else if (action == list)
  {
    Serial.printf_P(PSTR("Listing contents...\n"));
    uint16_t dirCount = 0;
    Dir dir = SPIFFS.openDir(F("/"));
    while (dir.next())
    {
      ++dirCount;
      Serial.printf_P(PSTR("%6ld  %s\n"), (uint32_t)dir.fileSize(), dir.fileName().c_str());
    }
    Serial.printf_P(PSTR("%d files total\n"), dirCount);
    action = show;
  }
}
