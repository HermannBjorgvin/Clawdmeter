# Clawdmeter com Claude Code, Codex e ESP32-2432S024C

[README em inglês](../README.md) ·
[Projeto Clawdmeter original](https://github.com/HermannBjorgvin/Clawdmeter)

Este repositório é um fork comunitário do Clawdmeter criado por Hermann
Bjorgvin. O projeto original criou o painel ESP32 para acompanhar o uso do
Claude Code e as animações pixel-art do Clawd. Este fork acrescenta:

- páginas separadas para Claude, Codex e Activity;
- informações locais de uso e atividade do Codex;
- transmissão USB serial e aplicativo de tray no Windows;
- suporte à placa capacitiva ESP32-2432S024C de 2,4 polegadas;
- builds portrait e landscape, com USB à esquerda no landscape;
- navegação para a esquerda e para a direita pelo touch.

O projeto não é oficial nem endossado pela Anthropic ou pela OpenAI.

![Animação splash do Clawd](../assets/demo.gif)

## O que aparece na tela

A interface possui três páginas:

1. **Claude** — percentual da sessão de cinco horas, uso semanal e horários de
   renovação.
2. **Codex** — até duas janelas de limite encontradas localmente, tokens usados
   no dia e plano, quando essas informações estiverem disponíveis.
3. **Activity** — sessões Claude Code em Open, Busy e Waiting, além de Codex
   Unread e o horário/estado da última varredura.

Toque na metade **esquerda** para voltar e na metade **direita** para avançar.
As páginas retornam circularmente. Sem toque, a tela avança a cada 12 segundos.
Depois de uma navegação manual, o ciclo automático fica suspenso por 30
segundos.

Quando uma fonte local não pode ser lida, a tela mostra `Unavailable`. Ela não
inventa um valor zero.

## Hardware testado

A placa validada é a **Sunton/Jingcai ESP32-2432S024C**:

- tela TFT colorida de 2,4 polegadas;
- resolução 240×320;
- touch capacitivo;
- ESP32 clássico com Wi-Fi e Bluetooth;
- alimentação, gravação e dados pela USB;
- slot microSD e GPIOs acessíveis.

Links úteis:

- [Referência técnica no CircuitPython](https://circuitpython.org/board/sunton_esp32_2432S024C/)
- [Exemplo de anúncio do modelo exato](https://www.amazon.com/dp/B0CLGD2DG6)
- [Busca do modelo no AliExpress](https://www.aliexpress.com/w/wholesale-esp32--2432s024c.html)
- [Waveshare ESP32-S3 AMOLED usada como referência pelo projeto original](https://www.waveshare.com/product/esp32-s3-touch-amoled-2.16.htm)

Essa placa TFT com ESP32 clássico costuma ser uma alternativa mais barata às
placas Waveshare AMOLED. O preço muda conforme vendedor, imposto e estoque, por
isso não há um valor fixo recomendado.

A economia traz algumas diferenças: resolução menor, tela TFT em vez de AMOLED,
menos memória e ausência dos periféricos integrados da Waveshare, como áudio,
IMU e gerenciamento avançado de bateria.

Confira o nome antes de comprar:

- `ESP32-2432S024C`: touch **capacitivo**, usado neste projeto;
- modelos terminados em `R`: normalmente touch **resistivo**;
- tamanhos 2,8, 3,2 e outros possuem pinagem e drivers diferentes.

## O que precisa existir em cada computador

A placa não lê os dados do Claude Code ou do Codex sozinha. Cada computador que
for alimentar o painel precisa executar o daemon/tray do projeto.

No Windows, você precisa de:

- Windows 10 ou 11 nativo;
- Python 3.11 ou superior;
- Claude Code instalado e autenticado com `claude login`;
- Codex instalado e já utilizado localmente para que existam sessões;
- este repositório;
- cabo USB capaz de transmitir dados;
- PlatformIO somente para compilar ou gravar firmware.

Se a placa já estiver gravada, trocar de computador **não exige gravar novamente
o firmware**. É necessário apenas instalar o tray no novo PC e usar um cabo USB
de dados.

## Instalação no Windows

Depois que o fork estiver publicado, abra o PowerShell:

```powershell
git clone https://github.com/Atzingen/Clawdmeter.git
cd Clawdmeter
git switch esp32-2432s024c-codex
powershell -ExecutionPolicy Bypass -File install-windows.ps1
```

O instalador:

1. cria o ambiente Python em `.venv`;
2. instala `pyserial`, `httpx` e as dependências do tray;
3. registra a inicialização automática no usuário atual do Windows;
4. inicia o aplicativo de tray sem janela de console.

O mesmo cabo USB alimenta a placa e transporta os dados. Bluetooth não é
necessário para o ESP32-2432S024C.

Para detalhes do instalador e execução manual, consulte o
[guia completo do daemon Windows](../daemon/README-windows.md).

## Compilar e gravar o firmware

Instale o [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
somente se precisar compilar ou gravar a placa.

### Portrait

```powershell
pio run -d firmware -e esp32_2432s024c -j 1
pio run -d firmware -e esp32_2432s024c -t upload --upload-port COM3
```

### Landscape com USB à esquerda

```powershell
pio run -d firmware -e esp32_2432s024c_landscape -j 1
pio run -d firmware -e esp32_2432s024c_landscape -t upload --upload-port COM3
```

Troque `COM3` pela porta identificada no Gerenciador de Dispositivos. O
firmware landscape e sua transformação de touch formam um conjunto; se a imagem
ou o toque estiverem invertidos, confirme primeiro qual ambiente foi gravado.

Não é necessário instalar PlatformIO para executar apenas o tray e enviar dados
a uma placa já gravada.

## Uso diário

O tray inicia automaticamente com o Windows. O ícone indica:

- verde: placa conectada;
- amarelo/âmbar: procurando a placa;
- vermelho: erro.

O daemon identifica somente portas seriais USB físicas e ignora portas COM de
Bluetooth. A atualização ocorre aproximadamente a cada 60 segundos.

Veja as últimas linhas do log:

```powershell
Get-Content $env:LOCALAPPDATA\Clawdmeter\daemon.log -Tail 30
```

Para fixar manualmente uma porta antes de iniciar o tray:

```powershell
$env:CLAWDMETER_SERIAL_PORT = "COM3"
```

## De onde vêm os dados

O daemon trabalha na própria máquina:

- uso do Claude: caminho autenticado já utilizado pelo daemon original;
- atividade Claude Code:
  `%USERPROFILE%\.claude\sessions`;
- uso e janelas do Codex:
  `%USERPROFILE%\.codex\sessions`;
- Codex Unread:
  `%USERPROFILE%\.codex\.codex-global-state.json`.

O ESP32 recebe somente valores agregados. Prompts, respostas, títulos, caminhos
de projetos e conteúdo das sessões não são enviados à placa.

Os arquivos locais do Codex são um formato interno, não uma API pública e
estável da OpenAI. Se o formato esperado mudar ou ainda não existir, os campos
do Codex aparecem como `Unavailable`, mas o Claude pode continuar funcionando.
Para instalação e informações do produto, consulte o
[repositório oficial do OpenAI Codex](https://github.com/openai/codex).

O símbolo Blossom usado na página Codex permanece monocromático conforme as
[orientações de marca da OpenAI](https://openai.com/brand/). Isso não representa
endosso oficial ao projeto.

## Solução de problemas

| Problema | Verificação e correção |
| --- | --- |
| `Clawdmeter USB serial not found` | Confirme que o cabo transmite dados, reconecte a placa e veja a porta no Gerenciador de Dispositivos. |
| O tray escolheu a porta errada | Defina `CLAWDMETER_SERIAL_PORT=COM3` antes de iniciar. |
| Claude retorna HTTP 401 | Execute `claude login` novamente e reinicie o tray. |
| Codex aparece como `Unavailable` | Use o Codex no PC e confirme a existência de `%USERPROFILE%\.codex\sessions`. O formato interno também pode ter mudado. |
| Activity não mostra Claude | Confirme que o Claude Code criou estados locais em `%USERPROFILE%\.claude\sessions`. |
| A placa acende, mas os dados não mudam | Instale/inicie o tray no PC; a alimentação USB sozinha não transmite as métricas. |
| Imagem ou touch invertido | Grave o ambiente correto. O landscape testado usa a USB à esquerda. |
| Dados pararam após trocar de PC | Instale o tray no novo computador. Não é necessário regravar uma placa já atualizada. |
| Métricas antigas continuam na tela | Veja o log do tray e confirme novos envios/ACKs; a placa preserva a última leitura quando não recebe dados. |

## Splash screens

O splash original do Clawdmeter foi preservado. As animações pixel-art mudam
conforme a utilização do Claude e alternam enquanto a tela de splash está
ativa. Os exemplos existentes no repositório foram renderizados em placas
AMOLED do projeto original e não devem ser confundidos com uma fotografia da
placa TFT deste fork:

| Splash 2,16 polegadas | Splash 1,8 polegada |
| :---: | :---: |
| ![Splash original de 2,16 polegadas](../screenshots/splash.png) | ![Splash original de 1,8 polegada](../screenshots/amoled_18/splash.png) |

As animações foram obtidas da biblioteca
[claudepix.vercel.app](https://claudepix.vercel.app), criada por
[@amaanbuilds](https://x.com/amaanbuilds).

## Créditos e licenciamento

- [Hermann Bjorgvin](https://github.com/HermannBjorgvin) e colaboradores pelo
  [Clawdmeter original](https://github.com/HermannBjorgvin/Clawdmeter);
- [@amaanbuilds](https://x.com/amaanbuilds) pelas animações pixel-art do Clawd;
- [Lucide](https://lucide.dev) pelos ícones;
- Anthropic e OpenAI pelas respectivas marcas e ativos visuais.

O repositório herda fontes proprietárias e ativos de mascote/marca sobre os
quais o projeto não possui direitos. Por esse motivo, o aviso de área cinzenta
de licenciamento do projeto original foi preservado e nenhuma nova licença foi
adicionada para reivindicar direitos sobre esses ativos.
