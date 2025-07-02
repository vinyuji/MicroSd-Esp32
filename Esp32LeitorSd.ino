// --- Inclusão de Bibliotecas ---//
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include "time.h"

// --- Configurações de Rede ---//

const char* ssid = "...";      
const char* password = "..."; 

// --- Configurações de Hardware ---//

const int pinoLDR = 34; // Pino para o sensor LDR 
const int pinoCS = 5;   // Pino CS) para o cartão SD

// --- Configurações de Tempo (NTP) --- //

const char* servidorNTP = "pool.ntp.org";
const long  gmtOffset_sec = -10800; // Horário de Brasília
const int   daylightOffset_sec = 0; // Sem horário de verão

// --- Variáveis Globais --- //

WebServer servidor(80);
File arquivoLog;
String dadosParaLog;
unsigned long tempoAnterior = 0;
const long intervaloGravacao = 10000; // Intervalo para gravar dados no SD

// --- Página HTML ---//

const char paginaHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>Monitor de Luminosidade ESP32</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin: 0; padding: 20px; background-color: #f4f4f4; }
    h1 { color: #333; }
    #graficoContainer { width: 90%; max-width: 800px; margin: 30px auto; padding: 20px; background-color: #fff; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }
    .btn {
      display: inline-block;
      margin-top: 20px;
      padding: 12px 25px;
      font-size: 16px;
      font-weight: bold;
      color: #fff;
      border: none;
      border-radius: 5px;
      text-decoration: none;
      transition: background-color 0.3s;
      cursor: pointer;
    }
    #btnDownload {
      background-color: #007BFF;
    }
    #btnDownload:hover {
      background-color: #0056b3;
    }
    #btnVerLog {
      background-color: #28a745;
      margin-left: 10px;
    }
    #btnVerLog:hover {
      background-color: #218838;
    }
  </style>
</head>
<body>
  <h1>Monitor de Luminosidade em Tempo Real</h1>
  <div id="graficoContainer">
    <canvas id="graficoLuminosidade"></canvas>
  </div>
  
  <a href="/baixar" id="btnDownload" class="btn" download="dados_luminosidade.txt">Baixar Arquivo de Log</a>
  <a href="/verlog" id="btnVerLog" class="btn">Ver Arquivo de Log</a>

<script>
  const ctx = document.getElementById('graficoLuminosidade').getContext('2d');
  const grafico = new Chart(ctx, {
    type: 'line',
    data: {
      labels: [],
      datasets: [{
        label: 'Luminosidade (0-4095)',
        data: [],
        borderColor: 'rgba(255, 159, 64, 1)',
        backgroundColor: 'rgba(255, 159, 64, 0.2)',
        borderWidth: 2,
        fill: true,
        tension: 0.4
      }]
    },
    options: {
      scales: {
        y: {
          beginAtZero: true,
          suggestedMax: 4095
        }
      },
      animation: {
        duration: 200 // Animação mais suave
      }
    }
  });

  function adicionarDadoAoGrafico(label, data) {
    grafico.data.labels.push(label);
    grafico.data.datasets.forEach((dataset) => {
      dataset.data.push(data);
    });
    // Limita o número de pontos no gráfico para 20
    if (grafico.data.labels.length > 20) {
      grafico.data.labels.shift();
      grafico.data.datasets[0].data.shift();
    }
    grafico.update();
  }

  // Busca novos dados a cada 2 segundos
  setInterval(function () {
    fetch('/dados')
      .then(response => response.json())
      .then(data => {
        adicionarDadoAoGrafico(data.hora, data.luminosidade);
      })
      .catch(error => console.error('Erro ao buscar dados:', error));
  }, 2000);
</script>

</body>
</html>
)rawliteral";

// --- Funções Auxiliares ---//

String obterDataHoraFormatada() {
  struct tm infoTempo;
  if (!getLocalTime(&infoTempo)) {
    return "Erro ao obter hora";
  }
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%H:%M:%S %d/%m/%Y", &infoTempo);
  return String(buffer);
}

// Função para iniciar o cartão SD
void iniciarCartaoSD() {
  Serial.println("Inicializando o cartão SD...");
  if (!SD.begin(pinoCS)) {
    Serial.println("Falha na inicialização do cartão SD! Verifique as conexões.");
    return;
  }
  uint8_t tipoCartao = SD.cardType();
  if (tipoCartao == CARD_NONE) {
    Serial.println("Nenhum cartão SD encontrado.");
    return;
  }
  Serial.println("Cartão SD inicializado com sucesso.");
}

// Função para adicionar uma linha de log ao arquivo no cartão SD
void registrarDadosNoCartao(fs::FS &fs, const char * caminho, const char * mensagem) {
  Serial.printf("Registrando no arquivo: %s\n", caminho);

  // Abre o arquivo em modo de adição
  File arquivo = fs.open(caminho, FILE_APPEND);
  if (!arquivo) {
    Serial.println("Falha ao abrir o arquivo para registro.");
    return;
  }
  if (arquivo.println(mensagem)) {
    Serial.println("Mensagem registrada com sucesso.");
  } else {
    Serial.println("Falha ao registrar a mensagem.");
  }
  arquivo.close();
}


// --- Funções de Manipulação do Servidor Web ---//

// Manipulador para a página principal (raiz)
void lidarComRaiz() {
  servidor.send(200, "text/html", paginaHTML);
}

// Manipulador para o endpoint de dados (usado pelo gráfico)
void lidarComDados() {
  int valorLDR = analogRead(pinoLDR);
  
  // Inverte o valor lido para que a luz alta corresponda a um valor alto
  valorLDR = 4095 - valorLDR;
  
  struct tm infoTempo;
  getLocalTime(&infoTempo);
  char bufferHora[10];
  strftime(bufferHora, sizeof(bufferHora), "%H:%M:%S", &infoTempo);
  
  String json = "{\"luminosidade\":" + String(valorLDR) + ", \"hora\":\"" + String(bufferHora) + "\"}";
  servidor.send(200, "application/json", json);
}

// Manipulador para o download do arquivo de log
void lidarComDownload() {
  File arquivo = SD.open("/dados_luminosidade.txt");
  if (!arquivo) {
    servidor.send(404, "text/plain", "Arquivo de log não encontrado.");
    return;
  }
  servidor.streamFile(arquivo, "text/plain");
  arquivo.close();
}

// --- NOVA FUNÇÃO ADICIONADA ---//

// Manipulador para visualizar o arquivo de log no navegador
void lidarComVerLog() {
  File arquivo = SD.open("/dados_luminosidade.txt");
  if (!arquivo) {
    servidor.send(404, "text/plain", "Arquivo de log não encontrado.");
    return;
  }

  // Cria uma página HTML simples para exibir o conteúdo do log
  String paginaLog = "<!DOCTYPE HTML><html><head><title>Log de Luminosidade</title><meta charset='UTF-8'><style>body { font-family: monospace; background-color: #f4f4f4; } a { font-family: Arial, sans-serif; }</style></head><body><h2>Conteúdo do Arquivo de Log</h2><pre>";
  
  // Lê o arquivo e adiciona o conteúdo à página
  while(arquivo.available()){
    paginaLog += arquivo.readStringUntil('\n') + "\n";
  }
  arquivo.close();

  paginaLog += "</pre><br><a href='/'>Voltar para o Gráfico</a></body></html>";

  servidor.send(200, "text/html", paginaLog);
}

// Manipulador para páginas não encontradas
void lidarComNaoEncontrado() {
  servidor.send(404, "text/plain", "404: Nao encontrado");
}


// --- Função de Configuração Principal (setup) --- //

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Configura a atenuação do ADC para a faixa completa de 0-3.6V (11dB).
  analogSetPinAttenuation(pinoLDR, ADC_11db);

  // Inicia o cartão SD
  iniciarCartaoSD();

  // Conecta ao Wi-Fi
  Serial.printf("Conectando a %s ", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado!");
  Serial.print("Endereço IP: ");
  Serial.println(WiFi.localIP());

  // Configura o tempo a partir do servidor NTP
  configTime(gmtOffset_sec, daylightOffset_sec, servidorNTP);
  Serial.println("Tempo configurado via NTP.");

  // --- ROTA ADICIONADA ---
  // Configura as rotas do servidor web
  servidor.on("/", lidarComRaiz);
  servidor.on("/dados", lidarComDados);
  servidor.on("/baixar", lidarComDownload);
  servidor.on("/verlog", lidarComVerLog); // Nova rota para visualizar o log
  servidor.onNotFound(lidarComNaoEncontrado);

  // Inicia o servidor web
  servidor.begin();
  Serial.println("Servidor HTTP iniciado.");
}

// --- Função de Loop Principal ---//

void loop() {
  // Mantém o servidor web funcionando
  servidor.handleClient();

  // código para ler o sensor e gravar no SD em intervalos definidos
  unsigned long tempoAtual = millis();
  if (tempoAtual - tempoAnterior >= intervaloGravacao) {
    tempoAnterior = tempoAtual;

    // Lê o valor do LDR
    int valorLDR = analogRead(pinoLDR);
    
    // Inverte o valor lido para que a luz alta corresponda a um valor alto
    valorLDR = 4095 - valorLDR;
    
    Serial.printf("Valor do LDR (invertido): %d\n", valorLDR);

    // Formata a string de dados para o log
    String dataHora = obterDataHoraFormatada();
    dadosParaLog = "Luminosidade: " + String(valorLDR) + ", Data/Hora: " + dataHora;

    // Grava os dados no cartão SD
    registrarDadosNoCartao(SD, "/dados_luminosidade.txt", dadosParaLog.c_str());
  }
}
