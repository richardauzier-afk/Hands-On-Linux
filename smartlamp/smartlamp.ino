// Defina os pinos de LED e LDR
// Defina uma variável com valor máximo do LDR (4095)
// Defina uma variável para guardar o valor atual do LED (10)
int ledPin = 2;
int ledValue = 10;

// Use um pino ADC válido no ESP32:
int ldrPin = 34;

// Valor máximo do ADC no ESP32 (12 bits = 4095)
int ldrMax = 4095;

// Definições para PWM no ESP32:
const int pwmChannel = 0;
const int pwmFreq = 5000;   // Frequência em Hz
const int pwmResolution = 8; // Resolução de 8 bits (0-255)

void setup() {
    Serial.begin(9600);

    // Configurar PWM no ESP32
    ledcSetup(pwmChannel, pwmFreq, pwmResolution);
    ledcAttachPin(ledPin, pwmChannel);

    pinMode(ldrPin, INPUT);

    Serial.printf("SmartLamp Initialized.\n");
    Serial.println("Digite um comando no Serial Monitor:");
}

void loop() {
    // Obtenha os comandos enviados pela serial e processe-os
    if (Serial.available() > 0) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        processCommand(command);
        ledUpdate();
    }
}

void processCommand(String command) {
    int spaceIndex = command.indexOf(' ');
    String cmd, valueStr;
    int value = 0;

    if (spaceIndex != -1) {
        cmd = command.substring(0, spaceIndex);
        valueStr = command.substring(spaceIndex + 1);
        value = valueStr.toInt();
    } else {
        cmd = command;
    }

    if (cmd == "SET_LED") {
        if (value >= 0 && value <= 100) {
            ledValue = value;
            Serial.println("RES SET_LED 1");
        } else {
            Serial.println("RES SET_LED -1");
        }
    } else if (cmd == "GET_LED") {
        Serial.print("RES GET_LED ");
        Serial.println(ledValue);
    } else if (cmd == "GET_LDR") {
        int ldrValue = ldrGetValue();
        Serial.print("RES GET_LDR ");
        Serial.println(ldrValue);
    } else {
        Serial.println("ERR Unknown command.");
    }
}

void ledUpdate() {
    int pwmValue = map(ledValue, 0, 100, 0, 255);
    ledcWrite(pwmChannel, pwmValue);
}

int ldrGetValue() {
    int rawValue = analogRead(ldrPin);
    int normalizedValue = (rawValue * 100) / ldrMax;
    if (normalizedValue > 100) normalizedValue = 100;
    if (normalizedValue < 0) normalizedValue = 0;
    return normalizedValue;
}
