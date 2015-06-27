#include <avr/wdt.h>
#include <SPI.h>
#include <Ethernet.h>
#include <Streaming.h>

#define WPPPS 1.25*3600 // watts per pules per seconds
#define MIN_DT 50
#define SECS 5
#define SERVER "nas"
#define PORT 80
#define PATH F("/strom/log.py?t=")<<unixTime(t)<<F("&pulses=")<<pulses<<F("&pps=")<<pps<<F("&watts=")<<watts
#define RESET 9
#define MAX_ERROR_COUNT 3
#define WEB_SERVER_BUFFER_SIZE 16
#define ETH_CS 10
#define SD_CS 4

#define LOCAL_CLIENT // use local ethernet clients instead of a single global on
#define PUSH_DATA
//#define WEB_SERVER
#define SD_LOGGER
//#define SERIAL_OUTPUT
#define LOGGING

#if defined(SD_LOGGER)
#define LOGSD(msg) {File f = SD.open("data.log", FILE_WRITE); if(f){f << msg << endl; f.close();}}
#else
#define LOGSD(msg)
#endif

#if defined(LOGGING)
#define LOG(msg) {Serial << msg << endl; LOGSD(msg)}
#define LOG_INFO(s)  {unsigned long t=millis(); LOG(F("INFO(")  << unixTime(t) << F("):") << F(s))}
#define LOG_ERROR(s) {unsigned long t=millis(); LOG(F("ERROR(") << unixTime(t) << F("):") << F(s))}
#else
#define LOG_INFO(s)
#define LOG_ERROR(s)
#endif


byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xEA };
IPAddress ip(192, 168, 222, 202);

#if defined(WEB_SERVER)
#include <WebServer.h>
WebServer webserver("", 80);
#endif

#if !defined(LOCAL_CLIENT)
EthernetClient client;
#endif

#if defined(SD_LOGGER)
#include <SD.h>
#endif


volatile unsigned long pulses = 0, pulse_time = 0;
unsigned long t0 = 0, p0 = 0, toffset = 0, tset = 0;
float pps = 0, watts = 0;
byte error_count = 0;
char nl[] = "\n";

void (*soft_reset)() = 0;

void hard_reset() {
  LOG_ERROR("reset");
  delay(2000);
  pinMode(RESET, OUTPUT);
  digitalWrite(RESET, LOW);
}

void error() {
  if (error_count++ > MAX_ERROR_COUNT) {
    hard_reset();
    delay(2000);
    soft_reset();
  }
}

void success() {
  error_count = 0;
}

void count() {
  unsigned long t = millis();
  if (t - pulse_time > MIN_DT) {
    pulse_time = t;
    pulses++;
  }
}


unsigned long webUnixTime(Client &client) {
  unsigned long time = 0;

  // Just choose any reasonably busy web server, the load is really low
  if (client.connect(SERVER, PORT))  {
    // Make an HTTP 1.1 request which is missing a Host: header
    // compliant servers are required to answer with an error that includes
    // a Date: header.
    client.print(F("GET / HTTP/1.1 \r\n\r\n"));

    char buf[5];			// temporary buffer for characters
    client.setTimeout(5000);
    if (client.find("\r\nDate: ") // look for Date: header
        && client.readBytes(buf, 5) == 5) // discard
    {
      unsigned day = client.parseInt();	   // day
      client.readBytes(buf, 1);	   // discard
      client.readBytes(buf, 3);	   // month
      int year = client.parseInt();	   // year
      byte hour = client.parseInt();   // hour
      byte minute = client.parseInt(); // minute
      byte second = client.parseInt(); // second

      int daysInPrevMonths;
      switch (buf[0])
      {
        case 'F': daysInPrevMonths =  31; break; // Feb
        case 'S': daysInPrevMonths = 243; break; // Sep
        case 'O': daysInPrevMonths = 273; break; // Oct
        case 'N': daysInPrevMonths = 304; break; // Nov
        case 'D': daysInPrevMonths = 334; break; // Dec
        default:
          if (buf[0] == 'J' && buf[1] == 'a')
            daysInPrevMonths = 0;		// Jan
          else if (buf[0] == 'A' && buf[1] == 'p')
            daysInPrevMonths = 90;		// Apr
          else switch (buf[2])
            {
              case 'r': daysInPrevMonths =  59; break; // Mar
              case 'y': daysInPrevMonths = 120; break; // May
              case 'n': daysInPrevMonths = 151; break; // Jun
              case 'l': daysInPrevMonths = 181; break; // Jul
              default: // add a default label here to avoid compiler warning
              case 'g': daysInPrevMonths = 212; break; // Aug
            }
      }

      // This code will not work after February 2100
      // because it does not account for 2100 not being a leap year and because
      // we use the day variable as accumulator, which would overflow in 2149
      day += (year - 1970) * 365;	// days from 1970 to the whole past year
      day += (year - 1969) >> 2;	// plus one day per leap year
      day += daysInPrevMonths;	// plus days for previous months this year
      if (daysInPrevMonths >= 59	// if we are past February
          && ((year & 3) == 0))	// and this is a leap year
        day += 1;			// add one day
      // Remove today, add hours, minutes and seconds this month
      time = (((day - 1ul) * 24 + hour) * 60 + minute) * 60 + second;
    }
  }
  delay(100);
  client.flush();
  client.stop();

  return time;
}



void adjustTime() {
#if defined(LOCAL_CLIENT)
  EthernetClient client;
#endif
  unsigned long tunix = 0;
  while (tunix == 0) {
    tunix = webUnixTime(client);
    if (!tunix) {
      LOG_ERROR("adjust time failed");
      error();
      delay(3000);
    } else {
      toffset = tunix - (tset = millis()) / 1000;
      success();
      LOG_INFO("time adjusted");
    }
  }
}

unsigned long unixTime(unsigned long &tms) {
  return tms / 1000 + toffset;
}

void printData(Print &s, unsigned long &t, unsigned long &pulses, float &pps, float &watts, char sep[]) {
  s << F("t=") << unixTime(t) << sep;
  s << F("pulses=") << pulses << sep;
  s << F("pps=") << pps << sep;
  s << F("watts=") << watts << nl;
}


#if defined(WEB_SERVER)
void web(WebServer &server, WebServer::ConnectionType type, char* query, bool complete) {
  if (!complete) {
    server.httpFail();
  } else {
    if (type == WebServer::HEAD)
      return;
    server.httpSuccess("text/plain", "Cache-Control: no-cache, no-store, must-revalidate\r\nPragma: no-cache\r\nExpires: 0\r\n");
    printData(server, t0, p0, pps, watts, nl);
  }
}
#endif

#if defined(PUSH_DATA)
void pushdata(unsigned long &t, unsigned long &pulses, float &pps, float &watts) {
#if defined(LOCAL_CLIENT)
  EthernetClient client;
#endif
  if (toffset == 0) {
    LOG_ERROR("clock not set");
    error();
  }
  if (client.connect(SERVER, PORT) == 1) {
    client << F("GET ") << PATH << F(" HTTP/1.1") << endl ;
    client << F("Host: ") << SERVER << endl;
    client << F("Connection: close") << endl;
    client << endl;
    if (!client.find("OK"))    {
      LOG_ERROR("push not OK");
      error();
    } else {
      success();
      LOG_INFO("push sent");
    }
  } else {
    LOG_ERROR("push connect failed");
    error();
  }
  delay(100);
  client.flush();
  client.stop();
}
#endif

#if defined(SD_LOGGER)
void initSD() {
  if (SD.begin(SD_CS)) {
    LOG_INFO("SD initialized");
  } else {
    LOG_ERROR("SD init failed");
  }
}

void logToSD(unsigned long &t, unsigned long &p, float &pps, float &watts) {
  File f = SD.open("data.log", FILE_WRITE);
  if (f) {
    printData(f, t, p, pps, watts, ", ");
    f.close();
    LOG_INFO("data written");
  }  else {
    LOG_ERROR("data write failed");
    initSD();
  }
}
#endif

void logData(unsigned long &t, unsigned long &p, float &pps, float &watts) {
#if defined(SERIAL_OUTPUT)
  printData(Serial, t, p, pps, watts, nl);
#endif
#if defined(PUSH_DATA)
  pushdata(t, p, pps, watts);
#endif
#if defined(SD_LOGGER)
  logToSD(t, p, pps, watts);
#endif
}

void setup() {
  pinMode(RESET, INPUT_PULLUP);
  wdt_enable(WDTO_8S);
#if defined(SERIAL_OUTPUT) || defined(LOGGING)
  Serial.begin(115200);
#endif
  LOG_INFO("starting");
  pinMode(2, INPUT_PULLUP);
  delay(1000);
  Ethernet.begin(mac, ip);
#if defined(WEB_SERVER)
  webserver.setDefaultCommand(&web);
  webserver.begin();
#endif
#if defined(SD_LOGGER)
  initSD();
#endif
  adjustTime();
  attachInterrupt(0, count, FALLING);
  sei();
  LOG_INFO("up");
}

void loop() {
  unsigned long t = millis();
  if (t - tset > 3600000) {
    adjustTime();
  }
  unsigned long t1 = pulse_time, p1 = pulses;
  float dt = (t1 - t0) / 1000.0;
  if (dt > SECS) {
    pps = (p1 - p0) / dt;
    watts = WPPPS * pps;
    if (t0 > 0) // do not log the first pulse
      logData(t1, p1, pps, watts);
    t0 = t1;
    p0 = p1;
  } else if (t - t0 > 60000) {
    watts = pps = 0;
  }
#if defined(WEB_SERVER)
  char buff[WEB_SERVER_BUFFER_SIZE];
  int len = WEB_SERVER_BUFFER_SIZE;
  webserver.processConnection(buff, len);
#endif
  wdt_reset();
}
