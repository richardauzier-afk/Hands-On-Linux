// Defina os pinos de LED e LDR
// Defina uma variável com valor máximo do LDR (4000)
// Defina uma variável para guardar o valor atual do LED (10)
int ledPin = 2;
int ledValue = 10;

int ldrPin = 36;
// Faça testes no sensor ldr para encontrar o valor maximo e atribua a variável ldrMax
int ldrMax = 4095;

void setup() {
    Serial.begin(9600);
    
    pinMode(ledPin, OUTPUT);
    pinMode(ldrPin, INPUT);
    
    Serial.printf("SmartLamp Initialized.\n");
    Serial.println("Digite um comando no Serial Monitor:");


}

// Função loop será executada infinitamente pelo ESP32
void loop() {
    //Obtenha os comandos enviados pela serial 
    //e processe-os com a função processCommand
    
    if(Serial.available()){
      String command = Serial.readStringUntil('\n');
      command.trim();
      processCommand(command);
      ledUpdate();
    }
    
}


void processCommand(String command) {
    // compare o comando com os comandos possíveis e execute a ação correspondente
    
    int spaceIndex = command.indexOf(' ');
    String cmd, valueStr;
    int value = 0;

    // Se houver argumento no comando, separe comando e valor
    if(spaceIndex != -1){
        cmd = command.substring(0, spaceIndex);
        valueStr = command.substring(spaceIndex + 1);
        value = valueStr.toInt();
    } 
    else{
        cmd = command; // Comando sem argumentos
    }

    if(cmd == "SET_LED"){
        if(value >= 0 && value <= 100){
            ledValue = value; // Atualiza a variável global
            Serial.println("RES SET_LED 1");
        } 
        else{
            Serial.println("RES SET_LED -1");
        }
    } 
    else if(cmd == "GET_LED"){
        Serial.print("RES GET_LED ");
        Serial.println(ledValue);
    } 
    else if (cmd == "GET_LDR"){
        int ldrValue = ldrGetValue();
        Serial.print("RES GET_LDR ");
        Serial.println(ldrValue);
    } 
    else{
        Serial.println("ERR Unknown command.");
    }
}

// Função para atualizar o valor do LED
void ledUpdate() {
    // Valor deve convertar o valor recebido pelo comando SET_LED para 0 e 255
    // Normalize o valor do LED antes de enviar para a porta correspondente
    int pwmValue = map(ledValue, 0, 100, 0, 255);
    analogWrite(ledPin, pwmValue);
}

// Função para ler o valor do LDR
int ldrGetValue() {
    // Leia o sensor LDR e retorne o valor normalizado entre 0 e 100
    // faça testes para encontrar o valor maximo do ldr (exemplo: aponte a lanterna do celular para o sensor)       
    // Atribua o valor para a variável ldrMax e utilize esse valor para a normalização
    int rawValue = analogRead(ldrPin);
    int normalizedValue = (rawValue * 100) / ldrMax;
    if(normalizedValue > 100) normalizedValue = 100;
    if(normalizedValue < 0) normalizedValue = 0;
    return normalizedValue;
}