#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ==========================================================
// DATOS PARA ACCESO A LA RED INALAMBRICA
// ==========================================================

#ifdef SIMULACION
const char* nombreRed = "Robelred";
const char* claveRed = "";
#else
const char* nombreRed = "micro";
const char* claveRed = "micro9898";
#endif

// ==========================================================
// PARAMETROS DEL SERVIDOR MQTT
// ==========================================================

const char* direccionBroker = "broker.emqx.io";
const uint16_t puertoBroker = 1883;

const char* usuarioBroker = "";
const char* passwordBroker = "";

const char* canalControl = "reaction_game/control";
const char* canalResultados = "reaction_game/times";
const char* canalMensajes = "topic/qos0";

// ==========================================================
// ASIGNACION DE GPIO
// ==========================================================

#define PIN_BOTON_A 4
#define PIN_BOTON_B 13

#define PIN_LED_A 2
#define PIN_LED_B 15

// ==========================================================
// COMUNICACION ESP32
// ==========================================================

WiFiClient redESP;
PubSubClient servidorMQTT(redESP);

// ==========================================================
// ETAPAS DEL JUEGO
// ==========================================================

enum class Etapa {
  INICIO,
  ESPERA_A,
  LED_ACTIVO,
  ESPERA_B,
  FINALIZADO
};

Etapa etapaActual = Etapa::INICIO;

// ==========================================================
// REGISTRO DE TIEMPOS
// ==========================================================

uint32_t marcaEncendido = 0;
uint32_t marcaLiberacion = 0;

uint32_t resultadoA = 0;
uint32_t resultadoB = 0;

uint32_t comienzoEspera = 0;
uint32_t retardoGenerado = 0;

// ==========================================================
// ESTRUCTURA UTILIZADA PARA FILTRAR LOS BOTONES
// ==========================================================

struct Boton {
  bool lecturaAnterior;
  bool valorFiltrado;
  bool estadoPrevio;
  uint32_t instanteCambio;
};

Boton botonA = {false, false, false, 0};
Boton botonB = {false, false, false, 0};

const uint32_t filtroTiempo = 2000;

// ==========================================================
// FUNCIONES DE ENTRADA Y SALIDA
// ==========================================================

bool leerBoton(uint8_t pin) {
  return digitalRead(pin) == LOW;
}

void activarIndicadorA() {
  digitalWrite(PIN_LED_A, HIGH);
}

void desactivarIndicadorA() {
  digitalWrite(PIN_LED_A, LOW);
}

void activarIndicadorB() {
  digitalWrite(PIN_LED_B, HIGH);
}

void desactivarIndicadorB() {
  digitalWrite(PIN_LED_B, LOW);
}

// ==========================================================
// ESTABILIZACION DE LA SEÑAL DE UN BOTON
// ==========================================================

bool procesarBoton(uint8_t pin, Boton &datos, uint32_t reloj) {

  bool lecturaActual = leerBoton(pin);

  if (lecturaActual != datos.lecturaAnterior) {
    datos.instanteCambio = reloj;
    datos.lecturaAnterior = lecturaActual;
  }

  if ((reloj - datos.instanteCambio) >= filtroTiempo) {
    datos.valorFiltrado = lecturaActual;
  }

  return datos.valorFiltrado;
}

// ==========================================================
// CONECTAR EL ESP32 AL ROUTER
// ==========================================================

void iniciarRed() {

  Serial.println();
  Serial.print("Buscando la red: ");
  Serial.println(nombreRed);

  WiFi.begin(nombreRed, claveRed);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Conexion WiFi establecida.");
}

// ==========================================================
// RECUPERAR LA CONEXION CON MQTT SI SE PIERDE
// ==========================================================

void verificarBroker() {

  while (!servidorMQTT.connected()) {

    Serial.print("Intentando acceder al broker...");

    String identificador = "SistemaJuego-";
    identificador += String((uint32_t)esp_random(), HEX);

    if (servidorMQTT.connect(
          identificador.c_str(),
          usuarioBroker,
          passwordBroker)) {

      Serial.println(" conectado.");

      servidorMQTT.subscribe(canalControl);

      servidorMQTT.publish(
        canalMensajes,
        "Sistema preparado para iniciar"
      );

    } else {

      Serial.print(" Error MQTT: ");
      Serial.println(servidorMQTT.state());

      delay(2000);
    }
  }
}

// ==========================================================
// TRANSMISION DE LOS DOS RESULTADOS OBTENIDOS
// ==========================================================

void mandarResultados() {

  char paquete[100];

  snprintf(
    paquete,
    sizeof(paquete),
    "{\"reaction1_ms\":%lu,\"reaction2_ms\":%lu}",
    (unsigned long)resultadoA,
    (unsigned long)resultadoB
  );

  servidorMQTT.publish(canalResultados, paquete);

  String mensaje = "Primer resultado: ";
  mensaje += resultadoA;
  mensaje += " ms - Segundo resultado: ";
  mensaje += resultadoB;
  mensaje += " ms";

  servidorMQTT.publish(
    canalMensajes,
    mensaje.c_str()
  );

  Serial.println("Informacion enviada al servidor MQTT.");
}

// ==========================================================
// CONFIGURACION DEL MICROCONTROLADOR
// ==========================================================

void setup() {

  Serial.begin(115200);

  delay(500);

  Serial.println();
  Serial.println("----- JUEGO DE VELOCIDAD DE REACCION -----");

  pinMode(PIN_BOTON_A, INPUT_PULLUP);
  pinMode(PIN_BOTON_B, INPUT_PULLUP);

  pinMode(PIN_LED_A, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);

  desactivarIndicadorA();
  desactivarIndicadorB();

  iniciarRed();

  randomSeed(esp_random());

  servidorMQTT.setServer(
    direccionBroker,
    puertoBroker
  );
}

// ==========================================================
// CICLO PRINCIPAL
// ==========================================================

void loop() {

  // Se asegura que el enlace MQTT permanezca disponible
  if (!servidorMQTT.connected()) {
    verificarBroker();
  }

  servidorMQTT.loop();

  uint32_t relojActual = micros();

  // Se obtiene el estado ya estabilizado de cada pulsador
  bool botonAActivo = procesarBoton(
    PIN_BOTON_A,
    botonA,
    relojActual
  );

  bool botonBActivo = procesarBoton(
    PIN_BOTON_B,
    botonB,
    relojActual
  );

  // Se identifica el momento exacto en que cambia el boton A
  bool pulsoA =
    botonAActivo && !botonA.estadoPrevio;

  bool liberarA =
    !botonAActivo && botonA.estadoPrevio;

  botonA.estadoPrevio = botonAActivo;

  // Para el segundo boton solo es necesario detectar la pulsacion
  bool pulsoB =
    botonBActivo && !botonB.estadoPrevio;

  botonB.estadoPrevio = botonBActivo;

  // ========================================================
  // CONTROL GENERAL DE LA SECUENCIA
  // ========================================================

  switch (etapaActual) {

    case Etapa::INICIO:

      desactivarIndicadorA();
      desactivarIndicadorB();

      if (pulsoA) {

        Serial.println(
          "Boton A detectado. No lo sueltes todavia."
        );

        comienzoEspera = relojActual;

        retardoGenerado = random(
          1500000,
          4500000
        );

        etapaActual = Etapa::ESPERA_A;
      }

      break;

    case Etapa::ESPERA_A:

      if (liberarA) {

        Serial.println(
          "El boton fue liberado antes de la señal."
        );

        etapaActual = Etapa::INICIO;

        break;
      }

      if ((relojActual - comienzoEspera)
          >= retardoGenerado) {

        activarIndicadorA();

        marcaEncendido = relojActual;

        Serial.println(
          "¡SEÑAL ACTIVADA! Suelta el Boton A."
        );

        etapaActual = Etapa::LED_ACTIVO;
      }

      break;

    case Etapa::LED_ACTIVO:

      if (liberarA) {

        marcaLiberacion = relojActual;

        resultadoA =
          (marcaLiberacion - marcaEncendido) / 1000;

        desactivarIndicadorA();

        Serial.print("Tiempo de respuesta 1: ");
        Serial.print(resultadoA);
        Serial.println(" ms");

        Serial.println(
          "Continua presionando el Boton B."
        );

        etapaActual = Etapa::ESPERA_B;
      }

      break;

    case Etapa::ESPERA_B:

      if (pulsoB) {

        uint32_t instanteB = relojActual;

        resultadoB =
          (instanteB - marcaLiberacion) / 1000;

        activarIndicadorB();

        Serial.print("Tiempo de respuesta 2: ");
        Serial.print(resultadoB);
        Serial.println(" ms");

        etapaActual = Etapa::FINALIZADO;
      }

      break;

    case Etapa::FINALIZADO:

      mandarResultados();

      delay(2000);

      desactivarIndicadorB();

      Serial.println(
        "Prueba completada. El sistema vuelve al inicio."
      );

      etapaActual = Etapa::INICIO;

      break;
  }
}