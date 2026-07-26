# PicoFlasher Corona Stable

Edição focada em leitura confiável das duas famílias Corona:

- NAND SPI de 16 MB;
- eMMC de 4 GB em modo MMC nativo de 1 bit;
- Raspberry Pi Pico padrão e RP2040-Zero;
- protocolo USB compatível com PicoFlasher 3/J-Runner.

## O que esta versão faz para evitar leituras incorretas

### Corona 16 MB

- Prepara SMC e SPI antes da enumeração USB, impedindo que o PC consulte a
  configuração enquanto a interface ainda está iniciando.
- Se a configuração retornar `0xFFFFFFFF`, reinicializa automaticamente
  SPI/FIFOs, repete a sequência de parada do SMC e tenta novamente até três
  ciclos. Não é mais necessário desconectar e conectar o RP2040.
- Usa cinco amostras por ciclo e aceita a configuração somente depois de duas
  amostras válidas idênticas.
- Lê cada página duas vezes.
- Se as duas cópias forem diferentes, faz uma terceira leitura.
- Só entrega a página quando pelo menos duas cópias, incluindo os 512 bytes
  principais e os 16 bytes spare, forem idênticas.
- Retorna `0x9001` quando não existe consenso; uma página duvidosa não é
  apresentada ao PC como se fosse válida.
- Lê três vezes o registro de configuração da NAND e também exige consenso.
- Usa SPI a 18 MHz por padrão, mais conservador que os 28 MHz anteriores.
- Usa timeouts em milissegundos reais e `memcpy` nos acessos de 32 bits,
  eliminando acessos potencialmente desalinhados.

### Corona 4 GB

- Inicializa a 375 kHz, calculados a partir do clock real do RP2040.
- Opera a 6,25 MHz por padrão, reproduzindo a frequência real na qual a versão
  recebida estava estável, sem depender de comentário ou overclock antigo.
- Valida o CRC7 das respostas de comando.
- Valida o CRC16 de cada setor de 512 bytes antes de entregá-lo ao PC.
- Repete automaticamente uma leitura até três vezes.
- Depois de falhas repetidas, reduz automaticamente o clock para 3,125 MHz e
  tenta novamente.
- Se PIO/eMMC perderem sincronismo, reinicializa completamente a eMMC e repete
  o mesmo setor antes de encerrar o stream.
- Uma nova leitura também reinicializa automaticamente a eMMC quando a sessão
  anterior terminou com erro, evitando a segunda leitura instantânea de 0 MB.
- Valida o status R1 da eMMC em comandos de leitura, escrita, seleção e estado.
- Todos os laços de espera de comando, PIO e DMA possuem timeout por tempo
  real; não há mais espera infinita nem `panic()` por CRC.
- PIO1 fica reservado ao eMMC. O áudio Nuvoton usa uma máquina de estados
  livre do PIO0, eliminando a colisão anterior com o clock SDIO.
- Remove a rotina antiga que alterava o DMA 3 sem reservá-lo e alternava o
  GPIO 0 durante transferências.

## Compilação

Requisitos: Pico SDK instalado, toolchain ARM GNU e CMake.

Defina primeiro o caminho do SDK:

```bash
export PICO_SDK_PATH=/caminho/para/pico-sdk
```

Raspberry Pi Pico padrão:

```bash
cmake -S . -B build-pico -DUSE_RP2040_ZERO=OFF
cmake --build build-pico -j
```

RP2040-Zero:

```bash
cmake -S . -B build-zero -DUSE_RP2040_ZERO=ON
cmake --build build-zero -j
```

O arquivo `.uf2` ficará dentro da pasta de build correspondente.

Para começar diretamente na frequência eMMC mais conservadora:

```bash
cmake -S . -B build-safe \
  -DUSE_RP2040_ZERO=OFF \
  -DEMMC_TRANSFER_HZ=3125000 \
  -DEMMC_FALLBACK_HZ=3125000
cmake --build build-safe -j
```

Parâmetros ajustáveis:

| Opção CMake | Padrão | Função |
|---|---:|---|
| `USE_RP2040_ZERO` | `OFF` | Seleciona a pinagem |
| `EMMC_TRANSFER_HZ` | 6.250.000 | Clock normal da eMMC |
| `EMMC_FALLBACK_HZ` | 3.125.000 | Clock após falhas/CRC |
| `XBOX_SPI_HZ` | 18.000.000 | Clock da NAND de 16 MB |
| `ENABLE_DEBUG_UART` | `OFF` | UART de diagnóstico |

Mesmo reutilizando uma pasta `build` antiga, o CMake limita automaticamente
valores armazenados acima de 6,25 MHz e fallback acima de 3,125 MHz. Ainda
assim, para uma comparação totalmente limpa, é recomendado criar uma nova
pasta de build.

## Pinagem

### Raspberry Pi Pico padrão

| Função | GPIO |
|---|---:|
| NAND SPI MISO | 16 |
| NAND SPI CS | 17 |
| NAND SPI CLK | 18 |
| NAND SPI MOSI | 19 |
| SMC DBG EN | 20 |
| SMC RESET | 21 |
| eMMC DAT0 | 6 |
| eMMC CMD | 7 |
| eMMC CLK | 8 |
| eMMC RESET | 9 |

### RP2040-Zero

| Função | GPIO |
|---|---:|
| NAND SPI MISO | 0 |
| NAND SPI CS | 1 |
| NAND SPI CLK | 2 |
| NAND SPI MOSI | 3 |
| SMC DBG EN | 4 |
| SMC RESET | 5 |
| eMMC DAT0 | 26 |
| eMMC CMD | 27 |
| eMMC CLK | 28 |
| eMMC RESET | 29 |

O Nuvoton/Sonus usa GPIO 11 a 15 nas duas placas.

## Códigos de erro adicionados

eMMC:

| Código | Significado |
|---:|---|
| `-2` | resposta ou status R1 inválido |
| `-3` | CRC inválido |
| `-4` | parâmetro inválido |
| `-5` | timeout |
| `-6` | eMMC não inicializada |
| `-7` | estado inesperado de PIO/DMA |

NAND 16 MB:

| Código | Significado |
|---:|---|
| `0x9001` | três leituras sem duas cópias idênticas |

## Observação elétrica importante

O firmware impede que vários tipos de falha sejam aceitos silenciosamente,
mas nenhum software consegue prometer literalmente 100% de sucesso se houver
fio longo, solda fria, ausência de terra comum, alimentação indevida ou dano
na NAND/eMMC. Para aproveitar as validações desta versão:

- mantenha CLK, CMD/DAT e SPI curtos;
- use terra comum firme;
- não alimente simultaneamente por pontos conflitantes;
- mantenha o console desligado conforme o procedimento do flasher;
- se uma instalação específica continuar marginal, compile a eMMC diretamente
  a 3,125 MHz.

Consulte `RELATORIO_TECNICO.md` para a revisão arquivo por arquivo.
