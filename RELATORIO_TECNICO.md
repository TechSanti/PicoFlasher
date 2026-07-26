# Relatório técnico da revisão

## Resultado

O projeto foi mantido compatível com o protocolo PicoFlasher 3: os códigos de
comando, a estrutura de 5 bytes (`cmd + lba`), os tamanhos de resposta e a
versão retornada ao software do PC continuam iguais. As mudanças se concentram
em integridade, timeout, isolamento dos periféricos e rejeição explícita de
dados instáveis.

“Leitura estável” nesta edição significa que o firmware não considera uma
transferência correta apenas porque bytes chegaram: a NAND de 16 MB precisa de
consenso entre leituras e a eMMC de 4 GB precisa passar no CRC recebido.

## Revisão por arquivo

### `main.c`

- A primeira parada do SMC e inicialização SPI acontecem antes de `tusb_init()`;
  assim, o J-Runner não consegue consultar a NAND antes de ela estar pronta.
- Um evento de desmontagem USB invalida o estado `smc_stopped`, obrigando nova
  preparação se o host reconectar sem cortar a alimentação do RP2040.
- O stream eMMC verifica se existe uma sessão válida antes do primeiro setor;
  se a sessão anterior falhou, mantém o SMC em reset e inicializa novamente a
  eMMC em vez de devolver erro imediato no setor zero.
- Removida a reconfiguração manual de `clk_peri`; o projeto usa o clock padrão
  estável configurado pelo Pico SDK.
- Mantida a versão USB `3` para não quebrar o software do PC.
- O cálculo do tamanho necessário do pacote agora inclui os 512 bytes de
  `EMMC_WRITE`. Antes, o comando podia ser retirado do FIFO antes de seu payload,
  desalinhando todos os comandos seguintes.
- O callback processa todos os comandos completos já presentes no FIFO.
- Cada bloco enviado pelo modo stream chama `tud_cdc_write_flush()`.
- Resultados de 32 bits são copiados sem conversão por ponteiro desalinhado.
- Um buffer estático alinhado substitui grandes buffers temporários na pilha.
- Detecção da eMMC usa sete amostras da linha CMD e decisão por maioria.
- CID/CSD/EXT_CSD são zerados quando não podem ser lidos, evitando vazamento de
  conteúdo antigo de pilha.
- Retornos das funções ISD1200 agora refletem sucesso ou falha real.
- A inicialização do LED WS2812 deixa de abortar o firmware se uma máquina de
  estados ou memória PIO não estiver disponível.

### `sdio.c` e `sdio.h`

- Acrescentados códigos distintos para timeout, não inicializado e erro de
  estado interno.
- O clock é definido por `sd_set_clock_hz()`: divisor =
  `clk_sys / (2 × frequência)`.
- A sequência de identificação roda a 375 kHz e a transferência normal a
  6,25 MHz. Esse é o clock real produzido pelo divisor 10 da versão recebida
  quando o RP2040 trabalha a 125 MHz.
- A eMMC recebe RCA 1 em CMD3; o valor 0, reservado/inicial, não é reutilizado.
- Cada comando crítico verifica o retorno e, nos comandos R1, os bits de erro.
- CMD1 tem limite de dois segundos em vez de laço infinito.
- Esperas de FIFO, DMA e posição da máquina PIO usam `time_us_64()`.
- Resposta de comando usa armazenamento alinhado; leituras/escritas de palavras
  não dependem do alinhamento de um vetor de bytes.
- CRC7 inválido retorna `SD_ERR_CRC`; não executa mais `panic()`.
- O CRC16-CCITT do setor é calculado e comparado com os 16 bits anexados pela
  eMMC.
- Uma leitura síncrona faz três tentativas; em seguida baixa automaticamente
  para 3,125 MHz e faz mais três.
- Se todas falharem, o firmware reseta e enumera novamente a eMMC, fixa o clock
  seguro e repete o mesmo setor. Isso evita encerrar uma imagem por uma perda
  transitória de sincronismo.
- Se a recuperação final falhar, a sessão é invalidada; a próxima leitura faz
  nova inicialização em vez de terminar imediatamente em 0 MB.
- Leitura de EXT_CSD recebe a mesma validação CRC e política de retentativa.
- Transferências assíncronas guardam um prazo e abortam DMA/PIO de forma
  controlada quando ele expira.
- Os canais DMA 8–11 e as máquinas PIO1 SM0–SM2 são reservados uma única vez.
- Removida `spoop()`, que escrevia diretamente no DMA3 sem reservá-lo.
- Removidos os pulsos de diagnóstico em GPIO0. Isso era especialmente perigoso
  no RP2040-Zero, onde GPIO0 é NAND MISO.
- CMD, DAT0 e CLK usam drive de 4 mA e slew rate lento; CMD/DAT0 usam pull-up e
  histerese; CLK fica sem pull.
- Escrita é limitada a um setor por operação; o caminho multi-setor experimental
  da base original não é exposto como se fosse confiável.

### `xbox.c` e `xbox.h`

- A sequência de parada/depuração do SMC agora desativa e reinicializa SPI em
  todos os ciclos, limpando estado antigo do periférico e de seus FIFOs.
- Uma configuração inválida aciona até dois ciclos completos adicionais de
  recuperação, equivalentes ao efeito que antes só era obtido desconectando e
  reconectando o RP2040.
- Cada ciclo usa até cinco amostras espaçadas e exige duas válidas idênticas.
- Separada a leitura física única da leitura pública validada.
- A leitura pública compara duas páginas completas; em divergência, usa uma
  terceira cópia e escolhe apenas a cópia inteira confirmada duas vezes.
- Se nenhuma dupla for idêntica, retorna `0x9001`.
- O registro de configuração também exige duas amostras iguais.
- Leituras totalmente em zero ou `0xFFFFFFFF` são rejeitadas como barramento
  ausente, reduzindo falso reconhecimento entre Corona 16 MB e 4 GB.
- Timeouts deixaram de ser contadores dependentes da velocidade do processador
  e passaram a milissegundos reais.
- Leituras e escritas de palavras usam `memcpy`, eliminando casts desalinhados.
- Parâmetros de buffer são validados.

### `spiex.c` e `spiex.h`

- Clock SPI passou de 28 MHz fixos para 18 MHz configuráveis.
- Formato SPI é configurado explicitamente.
- A tabela de inversão de bits é `static const`.
- Acesso ao valor de 32 bits usa `memcpy`.
- Inicialização e desinicialização são idempotentes.
- Ao desativar SPI, MISO/MOSI/CLK voltam a entradas, reduzindo disputa com o
  circuito do console.

### `pio_spi.c` e `pio_spi.h`

- A inicialização retorna falha em vez de assumir que PIO/SM sempre existe.
- A máquina de estados é reservada dinamicamente e liberada no encerramento.
- A existência de espaço para o programa PIO é verificada.
- A frequência agora é informada em hertz.
- Todas as transferências possuem timeout de um segundo sem progresso.
- Estado de inicialização e variante CPHA ficam registrados para remoção segura.

### `nuvoton_spi.c` e `nuvuton_spi.h`

- O Nuvoton saiu de PIO1 SM0, usado também pelo clock da eMMC.
- Agora usa uma SM livre de PIO0, compatível com o WS2812 do RP2040-Zero.
- Inicialização e transferência retornam `bool`.

### `isd1200.c` e `isd1200.h`

- Toda operação verifica se o barramento e o dispositivo estão inicializados.
- Power-up, comando, escrita e chip erase possuem timeouts.
- Falha de identificação executa limpeza de PIO antes de retornar.
- Endereços de 16/24 bits são montados byte a byte, sem casts desalinhados.
- Corrigido o erro de uma posição em `DIG_READ`: com três bytes de endereço, o
  primeiro dado recebido está em `buffer[4]`, não em `buffer[5]`.
- Leitura usa exatamente `1 + 3 + 512` bytes.
- Escrita limpa interrupções antigas, espera o busy terminar e rejeita
  `OVF_ERR`, `CMD_ERR` e `MPT_ERR`.
- Chip erase verifica erros após a conclusão.

### `CMakeLists.txt`

- Uma única árvore compila Pico padrão ou RP2040-Zero.
- UART de diagnóstico fica desligada por padrão, evitando conflito de pinos e
  tráfego inesperado.
- Frequências são opções de cache CMake.
- Ativados avisos de compilador úteis.
- Incluída explicitamente a biblioteca de clocks.

### `crc7.h`, `crc-itu-t.h`, `mmc_defs.h`

- Headers possuem guards e tabelas `static const`, evitando múltiplas
  definições.
- Os vetores foram conferidos: CMD0 produz byte CRC7 `0x95`; CRC16-CCITT com
  início zero para `123456789` produz `0x31C3`.
- Máscaras R1 de 32 bits usam literais sem sinal, removendo deslocamento assinado
  indefinido em `1 << 31`.

### `tusb_config.h` e `usb_descriptors.c`

- FIFOs CDC foram ajustados para 4096 bytes cada, suficientes para os pacotes do
  protocolo sem consumir 16 KB só em FIFO.
- O buffer de endpoint ficou em 512 bytes, múltiplo do endpoint USB FS de 64.
- `string.h` é incluído explicitamente para `memcpy` e `strlen`.

### Arquivos PIO e definições restantes

- `spi.pio`, `sdio.pio` e `ws2812.pio` mantêm sua lógica de temporização,
  evitando uma alteração desnecessária em código sensível a ciclo.
- `pins.h` mantém as duas pinagens recebidas.
- `sd.h` e as definições MMC não relacionadas ao caminho executado foram
  preservadas.

## Verificações executadas

- Estrutura sintática de todos os módulos C alterados analisada.
- Chaves, comentários e strings conferidos em todos os `.c` e `.h`.
- Manifesto de fontes do CMake conferido.
- CRC7 e CRC16 testados com vetores conhecidos.
- Códigos e tamanhos do protocolo USB comparados com a versão recebida.

O ambiente desta análise não contém o Pico SDK/toolchain ARM completo; por isso,
o pacote contém as fontes e a configuração de build, mas não um UF2 pré-compilado.
