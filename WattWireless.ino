
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

const char* ssid = "wi-fi.5-stars.net.ua";
const char* password = "";


ESP8266WebServer server(80);



const int CLKPin = 14; // Pin connected to CLK (D2 & INT0)
const int MISOPin = 12;  // Pin connected to MISO (D5)
const int RelePin = 5;  // Pin connected to MISO (D5)


const int SEND_INTERVAL = 60 * 60 * 1000; // 60 minutes

//All variables that are changed in the interrupt function must be volatile to make sure changes are saved.
volatile int Ba = 0;   //Store MISO-byte 1
volatile int Bb = 0;   //Store MISO-byte 2
volatile int Bc = 0;   //Store MISO-byte 2
float U = 0;    //voltage
float P = 0;    //power

volatile long CountBits = 0;      //Count read bits
volatile long ClkHighCount = 0;   //Number of CLK-highs (find start of a Byte)
volatile boolean inSync = false;  //as long as we ar in SPI-sync
volatile boolean NextBit = true;  //A new bit is detected

volatile unsigned int isrTriggers; // for debugging to see if ISR routine is being called

float avgVolts, minVolts, maxVolts;
float avgWatts, minWatts, maxWatts;
int numReadings;

unsigned long lastSend;

unsigned long debugOps;

String g_WattInfo = "";

//Check if header is present and correct
bool is_authentified() {
  Serial.println("Enter is_authentified");
  if (server.hasHeader("Cookie")) {
    Serial.print("Found cookie: ");
    String cookie = server.header("Cookie");
    Serial.println(cookie);
    if (cookie.indexOf("ESPSESSIONID=1") != -1) {
      Serial.println("Authentification Successful");
      return true;
    }
  }
  Serial.println("Authentification Failed");
  return false;
}

//login page, also called for disconnect
void handleLogin() {
  String msg;
  if (server.hasHeader("Cookie")) {
    Serial.print("Found cookie: ");
    String cookie = server.header("Cookie");
    Serial.println(cookie);
  }
  if (server.hasArg("DISCONNECT")) {
    Serial.println("Disconnection");
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("Set-Cookie", "ESPSESSIONID=0");
    server.send(301);
    return;
  }
  if (server.hasArg("USERNAME") && server.hasArg("PASSWORD")) {
    if (server.arg("USERNAME") == "admin" &&  server.arg("PASSWORD") == "admin") {
      server.sendHeader("Location", "/");
      server.sendHeader("Cache-Control", "no-cache");
      server.sendHeader("Set-Cookie", "ESPSESSIONID=1");
      server.send(301);
      Serial.println("Log in Successful");
      return;
    }
    msg = "Wrong username/password! try again.";
    Serial.println("Log in Failed");
  }

  String content = "<html><body><form action='/login' method='POST'>To log in, please use : admin/admin<br>";
  content += "User:<input type='text' name='USERNAME' placeholder='user name'><br>";
  content += "Password:<input type='password' name='PASSWORD' placeholder='password'><br>";
  content += "<input type='submit' name='SUBMIT' value='Submit'></form>" + msg + "<br>";
  content += "You also can go <a href='/inline'>here kik</a></body></html>";
  server.send(200, "text/html", content);

}

//root page can be accessed only if authentification is ok
void handleRoot() {
  Serial.println("Enter handleRoot");
  String header;
  if (!is_authentified()) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
    return;
  }

  String content = "<html><body><H2>hello, you successfully connected to esp8266!</H2><br><form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>";
  if (server.hasHeader("User-Agent")) {
    content += "the user agent used is : " + server.header("User-Agent") + "<br><br>";
  }
  content += "You can access this page until you <a href=\"/login?DISCONNECT=YES\">disconnect</a> <br/> <a href=\"/info\">info3</a> </body></html>";

  server.send(200, "text/html", content);
}




//no need authentification
void handleNotFound() {
  String message = "";
  message += "With searching comes loss\n";
  message += "and the presence of absence:\n";
  message += "\"";
  message += server.uri();
  message += "\"";
  message += " not found.\n\n";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}



void handleInfo() {
  String message = "Info\n\n";

  message += "Watt: ";
  message += g_WattInfo;
  server.send(200, "text/plain", message);
}


void clearTallys() {
  numReadings = 0;
  minVolts = 9999;
  maxVolts = -9999;
  minWatts = 9999;
  maxWatts = -9999;
}

void updateTallys(float volts, float watts) {

  // a running average calculation
  avgVolts = (volts + (numReadings * avgVolts)) / (numReadings + 1);
  avgWatts = (watts + (numReadings * avgWatts)) / (numReadings + 1);

  // min and max appear not to be working on ESP8266! See https://github.com/esp8266/Arduino/issues/398
  //  minVolts = min(minVolts, volts);
  //  maxVolts = max(maxVolts, volts);
  if (volts < minVolts) minVolts = volts;
  if (volts > maxVolts) maxVolts = volts;
  //  minWatts = min(minWatts, watts);
  //  maxWatts = max(maxWatts, watts);
  if (watts < minWatts) minWatts = watts;
  if (watts > maxWatts) maxWatts = watts;

  numReadings += 1;
  g_WattInfo = watts;
  Serial.print("Readings="); Serial.println(numReadings);
  Serial.print("Volts: "); Serial.print(volts); Serial.print(" avg="); Serial.print(avgVolts); Serial.print(" min="); Serial.print(minVolts); Serial.print(" max="); Serial.println(maxVolts);
  Serial.print("Watts: "); Serial.print(watts); Serial.print(" avg="); Serial.print(avgWatts); Serial.print(" min="); Serial.print(minWatts); Serial.print(" max="); Serial.println(maxWatts);
}


void doInSync() {
  CountBits = 0;  //CLK-interrupt increments CountBits when new bit is received
  while (CountBits < 40) {} //skip the uninteresting 5 first bytes
  CountBits = 0;
  Ba = 0;
  Bb = 0;
  while (CountBits < 24) { //Loop through the next 3 Bytes (6-8) and save byte 6 and 7 in Ba and Bb
    if (NextBit == true) { //when rising edge on CLK is detected, NextBit = true in in interrupt.
      if (CountBits < 9) { //first Byte/8 bits in Ba
        Ba = (Ba << 1);  //Shift Ba one bit to left and store MISO-value (0 or 1) (see http://arduino.cc/en/Reference/Bitshift)
        //read MISO-pin, if high: make Ba[0] = 1
        if (digitalRead(MISOPin) == HIGH) {
          Ba |= (1 << 0); //changes first bit of Ba to "1"
        }   //doesn't need "else" because BaBb[0] is zero if not changed.
        NextBit = false; //reset NextBit in wait for next CLK-interrupt
      }
      else if (CountBits < 17) { //bit 9-16 is byte 7, stor in Bb
        Bb = Bb << 1;  //Shift Ba one bit to left and store MISO-value (0 or 1)
        //read MISO-pin, if high: make Ba[0] = 1
        if (digitalRead(MISOPin) == HIGH) {
          Bb |= (1 << 0); //changes first bit of Bb to "1"
        }
        NextBit = false; //reset NextBit in wait for next CLK-interrupt
      }
    }
  }
  if (Bb != 3) { //if bit Bb is not 3, we have reached the important part, U is allready in Ba and Bb and next 8 Bytes will give us the Power.

    //Voltage = 2*(Ba+Bb/255)
    U = 2.0 * ((float)Ba + (float)Bb / 255.0);

    //Power:
    CountBits = 0;
    while (CountBits < 40) {} //Start reading the next 8 Bytes by skipping the first 5 uninteresting ones

    CountBits = 0;
    Ba = 0;
    Bb = 0;
    Bc = 0;
    while (CountBits < 24) { //store byte 6, 7 and 8 in Ba and Bb & Bc.
      if (NextBit == true) {
        if (CountBits < 9) {
          Ba = (Ba << 1);  //Shift Ba one bit to left and store MISO-value (0 or 1)
          //read MISO-pin, if high: make Ba[0] = 1
          if (digitalRead(MISOPin) == HIGH) {
            Ba |= (1 << 0); //changes first bit of Ba to "1"
          }
          NextBit = false;
        }
        else if (CountBits < 17) {
          Bb = Bb << 1;  //Shift Ba one bit to left and store MISO-value (0 or 1)
          //read MISO-pin, if high: make Ba[0] = 1
          if (digitalRead(MISOPin) == HIGH) {
            Bb |= (1 << 0); //changes first bit of Bb to "1"
          }
          NextBit = false;
        }
        else {
          Bc = Bc << 1;  //Shift Bc one bit to left and store MISO-value (0 or 1)
          //read MISO-pin, if high: make Bc[0] = 1
          if (digitalRead(MISOPin) == HIGH) {
            Bc |= (1 << 0); //changes first bit of Bc to "1"
          }
          NextBit = false;
        }
      }

    }

    //Power = (Ba*255+Bb)/2
    P = ((float)Ba * 255 + (float)Bb + (float)Bc / 255.0) / 2;

    if (U > 200 && U < 300 && P >= 0 && P < 4000) { // ignore spurious readings with voltage or power out of normal range
      updateTallys(U, P);
    } else {
      Serial.print(".");
    }

    inSync = false; //reset sync variable to make sure next reading is in sync.
  }

  if (Bb == 0) { //If Bb is not 3 or something else than 0, something is wrong!
    inSync = false;
    Serial.println("Nothing connected, or out of sync!");
  }
}

void CLK_ISR() {
  isrTriggers += 1;
  //if we are trying to find the sync-time (CLK goes high for 1-2ms)
  if (inSync == false) {
    ClkHighCount = 0;
    //Register how long the ClkHigh is high to evaluate if we are at the part wher clk goes high for 1-2 ms
    while (digitalRead(CLKPin) == HIGH) {
      ClkHighCount += 1;
      delayMicroseconds(30);  //can only use delayMicroseconds in an interrupt.
    }
    //if the Clk was high between 1 and 2 ms than, its a start of a SPI-transmission
    if (ClkHighCount >= 33 && ClkHighCount <= 67) {
      inSync = true;
    }
  }
  else { //we are in sync and logging CLK-highs
    //increment an integer to keep track of how many bits we have read.
    CountBits += 1;
    NextBit = true;
  }
}

boolean sendReading() {
  detachInterrupt(digitalPinToInterrupt(CLKPin));

  boolean sentOk = false;

  attachInterrupt(digitalPinToInterrupt(CLKPin), CLK_ISR, RISING);
  return sentOk;
}


void startWEBServer(void){
  const char * headerkeys[] = {"User-Agent", "Cookie"} ;
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
  
  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  server.on("/info", handleInfo); 
  server.onNotFound(handleNotFound);  
  
  server.on("/inline", []() {
    server.send(200, "text/plain", "this works without need of authentification");
  });
  
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.setDebugOutput(true);
      WiFiUDP::stopAll();
      Serial.printf("Update: %s\n", upload.filename.c_str());
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSketchSpace)) { //start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
      Serial.setDebugOutput(false);
    }
    yield();
  });


  //ask server to track these headers
  server.collectHeaders(headerkeys, headerkeyssize);
  server.begin();
  Serial.println("HTTP server started");
	
	
}

//----------------------------- S E T U P ---------------
void setup(void) {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // Trying connect to WiFi
  Serial.println("");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Start WEBServer  
  startWEBServer();

  // SPI work init
  attachInterrupt(digitalPinToInterrupt(CLKPin), CLK_ISR, RISING);
  pinMode(CLKPin, INPUT);
  pinMode(MISOPin, INPUT);
  clearTallys();
  // RelePin init

  pinMode(RelePin, OUTPUT);
  digitalWrite(RelePin, LOW);
}


//----------------------------- L O O P ----------------
void loop(void) {
  server.handleClient();

  if ((millis() - lastSend) > SEND_INTERVAL) {
    if (sendReading()) {
      clearTallys();
    }
    lastSend = millis();
  }

  if ((millis() - debugOps) > 10000) { // debug output every 10 secs to see its running
    Serial.print("ISR triggers = "); Serial.println(isrTriggers);
    debugOps = millis();
  }

  //do nothing until the CLK-interrupt occures and sets inSync=true
  if (inSync == true) {
   doInSync();
  }
}

