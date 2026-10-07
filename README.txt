# 🌿 HydroMonitor S3 — Sistema de Monitoramento Hidropônico Embarcado

O **HydroMonitor S3** é um sistema de telemetria e monitoramento local de baixo custo desenvolvido para sistemas hidropônicos horizontais e verticais. O projeto utiliza um microcontrolador **ESP32-S3** integrado a sensores de vazão, temperatura e nível líquido para prevenir falhas críticas no fornecimento de solução nutritiva.

---

## 📸 Contexto e Motivação

O projeto foi concebido após um incidente real de falha mecânica na bomba de circulação durante uma ausência prolongada, resultando na perda total da cultura hidropônica por desidratação. 

Para eliminar esse ponto único de falha (*Single Point of Failure*), foi desenvolvida uma arquitetura de monitoramento ativo capaz de validar o fluxo de água, checar a temperatura da solução e alertar sobre o nível crítico do reservatório em tempo real.

---

## 📐 Arquitetura Hidráulica e Estrutural

O sistema opera sob o princípio de circulação em circuito fechado (*NFT - Nutrient Film Technique* modificado):

* **Tubulação de Cultivo:** 5 tubos de PVC esgoto de 100 mm interligados em cascata e fixados à parede.
* **Reservatório Principal:** Bombona plástica de 50 Litros para acondicionamento da solução nutritiva (água + nutrientes).
* **Bomba de Circulação:** Bomba submersa **Sarlo Better SB2700** responsável pela elevação da água do reservatório até a entrada do tubo superior.
* **Retorno por Gravidade:** A água percorre a cascata de tubos e retorna ao reservatório no final do circuito.

### Módulo Adaptador de Sensores (Submerso / Tampa)
Para garantir uma instalação vedada, modular e de fácil manutenção, foi desenvolvido um suporte mecânico fixado à tampa do reservatório:

1. **Flange de Interface:** Fixada na tampa, estende-se internamente através de um tubo de PVC de 50 mm finalizado em uma redução $100 \times 50\text{ mm}$ com cap de 100 mm.
2. **Conexões de Passagem e Fluxo:**
   * O cap possui uma flange de $1/2''$ conectada externamente à bomba **SB2700**.
   * Internamente, conecta-se ao sensor de fluxo **YF-S201** e segue via mangueira flexível até a tubulação principal.
3. **Passagem de Sensores:**
   * Um prensa-cabos no cap garante a passagem estanque do sensor de temperatura **DS18B20**.
   * O sensor capacitivo de nível **XKC-Y25-V** é colado na parede interna do cap, detectando a ausência de líquido no limite inferior de operação.

---

## ⚡ Arquitetura Eletrônica e Hardware

O processamento e controle da instrumentação são centralizados no microcontrolador **ESP32-S3**.

### Especificações dos Componentes

| Componente | Tipo / Modelo | Função no Sistema | Interface / Protocolo |
| :--- | :--- | :--- | :--- |
| **Microcontrolador** | ESP32-S3 | Aquisição de dados, lógica e alertas | Wi-Fi / BLE / GPIO |
| **Sensor de Vazão** | YF-S201 | Validação de fluxo e detecção de pane na bomba | Sinal Pulsado (Efeito Hall) |
| **Sensor de Nível** | XKC-Y25-V | Sinalização de nível crítico no reservatório | Digital (HIGH / LOW) |
| **Sensor de Temp.** | DS18B20 | Leitura da temperatura da solução nutritiva | Digital (1-Wire) |
| **Conectividade** | Cabo Comando (20 vias) | Interconexão entre reservatório e gabinete | Analógico / Digital |
| **Interface Mecânica** | Conector ATX 24 Pinos | Desconexão rápida do painel do controlador | Plug-and-play |
| **Gabinete** | Impressão 3D | Alojamento do ESP32-S3 e proteção contra umidade | Customizado |

---

## 🔄 Fluxo de Operação

```
  +------------------------------------------------------------------+
  |                     RESERVATÓRIO (50 Litros)                     |
  |                                                                  |
  |   +-------------------+      +-------------------------------+   |
  |   |  Bomba SB2700     | ---> | Sensor de Vazão (YF-S201)     |   |
  |   +-------------------+      +-------------------------------+   |
  |   |  Sensor de Nível  |      | Sensor Temp. (DS18B20)        |   |
  |   |  (XKC-Y25-V)      |      |                               |   |
  |   +-------------------+      +-------------------------------+   |
  +-------------------------------------+----------------------------+
                                        |
                            Cabo Comando (20 Vias)
                             Conector ATX 24 Pinos
                                        |
                                        v
                            +-----------------------+
                            |     ESP32-S3 MCU      |
                            |                       |
                            | - Validação de Fluxo  |
                            | - Leitura de Temp.    |
                            | - Alerta de Nível     |
                            +-----------------------+
```

---

## 🛠️ Montagem e Manutenção

1. **Modularidade:** O uso do conector **ATX de 24 pinos** permite desacoplar totalmente o reservatório da unidade central de processamento (ESP32-S3) sem a necessidade de dessoldar cabos durante rotinas de limpeza ou troca de solução.
2. **Proteção Estrutural:** Peças customizadas foram desenvolvidas em **Impressora 3D** para acomodação da eletrônica de controle e suporte dos conectores de passagem.

---

## 🚀 Próximos Passos (Roadmap)

- [ ] Integração com broker MQTT (Home Assistant / Node-RED).
- [ ] Implementação de notificações via Telegram/WhatsApp em caso de falta de fluxo.
- [ ] Inclusão de sensores adicionais (pH e Condutividade Elétrica - EC).