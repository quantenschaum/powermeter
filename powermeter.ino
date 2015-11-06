#include <avr/wdt.h>
#include <SPI.h>
#include <Ethernet.h>
#include <Streaming.h>
#include <Time.h>

#define WPPPS 1.25*3600 // watts per pules per seconds
#define MIN_TIME 1446841865 // minimum valid time (UTC, secs sincs epoch) 
#define TIMEZONE 0 // offset in hours
#define TIME_RESYNC 600 // timeout in s after which the time is pulled from the net
#define MIN_DT 50
#define SECS 5
#define SERVER "nas"
#define PORT 80
#define PATH F("/strom/log.py?t=")<<t<<F("&pulses=")<<pulses<<F("&pps=")<<pps<<F("&watts=")<<watts
#define MAX_ERROR_COUNT 3
#define WEB_SERVER_BUFFER_SIZE 16
#define ETH_CS 10
#define SD_CS 4

#define LOCAL_CLIENT // use local ethernet clients instead of a single global on
//#define PUSH_DATA
#define WEB_SERVER
//#define SD_LOGGER
//#define SERIAL_OUTPUT 115200
//#define LOGGING

#if defined(SD_LOGGER)
#define LOGSD(msg) {File f = SD.open("data.log", FILE_WRITE); if(f){f << msg << endl; f.close();}}
#else
#define LOGSD(msg)
#endif

#if defined(LOGGING)
#define LOG(msg) {Serial << msg << endl; LOGSD(msg)}
#define LOG_INFO(s)  {LOG(F("INFO(")  << now() << F("):") << F(s))}
#define LOG_ERROR(s) {LOG(F("ERROR(") << now() << F("):") << F(s))}
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
unsigned long t0 = 0, p0 = 0;
float pps = 0, watts = 0;
byte error_count = 0;
char nl[] = "\n";

void (*soft_reset)() = 0;

#define NTP_PACKET_SIZE 48 // NTP time is in the first 48 bytes of message

time_t getNtpTime() {
  EthernetUDP Udp;
  Udp.begin(8888);
  while (Udp.parsePacket() > 0) ; // discard any previously received packets
  byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets
#if defined(NTP_SERVER)
  IPAddress NTP_SERVER;
#else
  IPAddress ntpIp = Ethernet.gatewayIP();
#endif
  sendNTPpacket(Udp, packetBuffer, ntpIp);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      time_t t = secsSince1900 - 2208988800UL;
      return t > MIN_TIME ? t + TIMEZONE * SECS_PER_HOUR : 0;
    }
  }
  return 0; // return 0 if unable to get the time
}

// send an NTP request to the time server at the given address
void sendNTPpacket(EthernetUDP &Udp, byte packetBuffer[], IPAddress &address) {
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}



void error() {
  LOG_ERROR("new error");
  if (error_count++ > MAX_ERROR_COUNT) {
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


void printData(Print &s, unsigned long &t, unsigned long &pulses, float &pps, float &watts, char sep[]) {
  s << F("t=") << t << sep;
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
    server.httpSuccess("text/plain", "Cache-Control: no-cache\r\n");
    unsigned long t1 = pulse_time, p1 = pulses;
    time_t ts = msToTime(t1);
    printData(server, ts, p1, pps, watts, nl);
  }
}
#endif

#if defined(PUSH_DATA)
void pushdata(time_t &t, unsigned long &pulses, float &pps, float &watts) {
#if defined(LOCAL_CLIENT)
  EthernetClient client;
#endif
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

void logToSD(time_t &t, unsigned long &p, float &pps, float &watts) {
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

time_t msToTime(unsigned long &ms) {
  return now() - (millis() - ms) / 1000ul;
}

void logData(unsigned long &t, unsigned long &p, float &pps, float &watts) {
  time_t ts = msToTime(t);

  if (timeStatus() == timeNotSet) {
    LOG_ERROR("clock not set");
    error();
  }

#if defined(SERIAL_OUTPUT)
  printData(Serial, ts, p, pps, watts, nl);
#endif
#if defined(PUSH_DATA)
  pushdata(ts, p, pps, watts);
#endif
#if defined(SD_LOGGER)
  logToSD(ts, p, pps, watts);
#endif
}

void setup() {
  wdt_enable(WDTO_8S);
#if defined(SERIAL_OUTPUT) || defined(LOGGING)
  Serial.begin(SERIAL_OUTPUT);
#endif
  LOG_INFO("starting");
  pinMode(2, INPUT_PULLUP);
  Ethernet.begin(mac, ip);
  setSyncProvider(getNtpTime);
  setSyncInterval(TIME_RESYNC);
#if defined(WEB_SERVER)
  webserver.setDefaultCommand(&web);
  webserver.begin();
#endif
#if defined(SD_LOGGER)
  initSD();
#endif
  attachInterrupt(0, count, FALLING);
  sei();
  LOG_INFO("up");
}

void loop() {
  unsigned long t = millis();
  unsigned long t1 = pulse_time, p1 = pulses;
  float dt = (t1 - t0) / 1000.;
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
  webserver.processConnection(buff, &len);
#endif

  wdt_reset();
}
