# Pokémon Liquid Crystal — Batalhas em Tempo Real

O **Pokémon Liquid Crystal** (beta 3.3) é um hack binário do FireRed 1.0 (BPRE): não tem código-fonte. Por isso, nesta versão o motor de batalha em tempo real **é injetado na ROM**: o mesmo código C desta branch (`src/rt_*.c`) é compilado e ligado contra os endereços do FireRed 1.0 original, colocado depois dos 16 MB da ROM (em `0x09000000`) e alguns pontos da ROM são desviados para ele.

> Este repositório não contém nenhuma ROM. Para gerar a versão em tempo real é preciso ter a ROM do Liquid Crystal 3.3.

## O que vem do mod

- Batalhas em tempo real: selvagens, treinadores, líderes e duplas (o Safari e a Trainer Tower seguem as regras do FireRed onde existirem). As arenas saem dos mapas de Johto/Kanto do próprio Liquid Crystal (conferidas em 207 mapas com encontros).
- **Físico/especial por golpe:** o Liquid Crystal tem a divisão física/especial (byte 10 de cada golpe); o motor usa essa divisão no dano, no Counter/Mirror Coat, no Hustle etc.
- **Pokémon de Sinnoh:** os 25 que o Liquid Crystal coloca nos espaços vazios (Turtwig … Bastiodon) têm sprites animados próprios do SpriteCollab, com paleta shiny derivada das paletas do próprio jogo.
- Menu **OPTION** com DIFFICULTY, AUTO AIM, EXP SHARE, EXP BONUS e BATTLE SPEED (a linha BUTTON MODE some: o Liquid Crystal sempre usa L/R como botões de página, e no tempo real L/R são golpes).
- Tela de resumo com o **cooldown** de cada golpe em segundos no lugar do PP.
- **MOVES no menu da equipe:** abre o Move Relearner do próprio jogo para o Pokémon escolhido (golpes de nível até o nível atual e os egg moves da forma básica da família; até 24 golpes) e volta para a equipe ao sair.
- **HMs pela bolsa:** basta ter a HM na bolsa (e a insígnia que o jogo pede para o golpe). Cut, Surf, Strength, Rock Smash, Waterfall, Dive etc. funcionam sem nenhum Pokémon saber o golpe (o primeiro Pokémon apto da equipe faz a animação); FLASH e STRENGTH aparecem no menu de qualquer Pokémon; as cavernas escuras já se iluminam ao entrar se a HM05 estiver na bolsa (com a primeira insígnia); e golpes de HM podem ser esquecidos como qualquer outro. O Fly segue o sistema do próprio Liquid Crystal.
- **Centro Pokémon instantâneo:** falar com a enfermeira cura na hora, sem conversa e sem a animação das Pokébolas.
- Captura mais fácil, POKé BALL infinita, painel de EXP, Leftovers e itens em tempo real, Snore/Sleep Talk, equilíbrio corpo a corpo × distância, velocidade da batalha — tudo como no FireRed.
- O sistema de dia/noite do Liquid Crystal é pausado durante a batalha (senão os sprites e o HUD ficariam escurecidos) e volta ao sair.

- Menu **OPTION** também com **LEVEL CAP** e **AUTO HEAL** (a lista rola). Com o LEVEL CAP, o nível máximo é o do Pokémon mais forte do líder mais fácil ainda não vencido, com os times do próprio Liquid Crystal: Falkner 17, Bugsy 22, Whitney 25, Morty 29, Chuck 38, Jasmine 41, Pryce 42, Clair 44; depois das 8 insígnias de Johto, 61 (Lance); depois do Hall da Fama, Kanto em qualquer ordem (Sabrina 54, Surge 55, Erika 56, Janine 58, Misty 63, Brock/Blaine/Blue 65) e sem limite com as 16. No limite o Pokémon não ganha EXP nas batalhas e o RARE CANDY não tem efeito. O AUTO HEAL cura a equipe no fim de toda batalha que não foi perdida.
- Shiny 1/64; TMs reutilizáveis; a fanfarra de item toca até o fim sem prender o jogador e sem a mensagem extra de "guardou na bolsa".
- **Goldenrod Dept. Store (4F, balcão das vitaminas):** vende todas as pedras de evolução (Fire, Water, Thunder, Leaf, Moon, Sun). O Liquid Crystal não tem evoluções por troca (todas já são por nível ou pedra).
- Tela de resumo, página SKILLS: **A** alterna entre atributos, IVs, EVs e **talentos** (Lv 20/40/60, um de dois por nível; SELECT escolhe o nível, START troca). Os talentos ficam em bits de fitas sem uso.
- Batalha: barra de stamina do dash, HUD mínimo transparente com o campo em tela cheia, buffer de comandos, cooldown global de 18 quadros, pulso de impacto, sprites grandes (Onix, Steelix, Lugia, Ho-Oh…) desenhados ampliados em vez de cortados, estilos de IA dos treinadores e modificadores de arena raros (12%): erupções em Cinnabar, neblina no tempo nublado / Ilex Forest / Lake of Rage / Tin Tower, vento no mar.
- O dia/noite continua o do próprio Liquid Crystal (pelo relógio do cartucho); o relógio acelerado do FireRed não é usado aqui.

Não fazem parte desta versão (o Liquid Crystal tem os seus próprios): a redistribuição da Pokédex, os lendários do mod e as evoluções sem troca do FireRed.

## Como gerar a ROM

```bash
make firered_modern -j$(nproc)          # uma vez, para gerar cabeçalhos e ferramentas
tools/lc/build.sh                        # build/lc/rt_blob.bin + rt_blob.map
python3 tools/lc/patch.py "Pokemon Liquid Crystal 3.3.gba" build/lc/rt_blob.bin build/lc/rt_blob.map saida.gba
```

A ROM resultante tem ~20,6 MB (o motor e os sprites ocupam ~4,7 MB depois dos 16 MB originais). O formato do save é o mesmo (Flash 128K); saves existentes do Liquid Crystal devem continuar funcionando, com as opções novas no padrão.

## Como funciona

| Arquivo | Papel |
|---|---|
| `tools/lc/gen_syms.py` | Gera `syms.ld`: todos os símbolos do FireRed 1.0 (`pokefirered_bpre10.sym`, da branch `symbols` do pret) nos seus endereços, com o bit Thumb nas funções; tabelas que o Liquid Crystal moveu (`overrides.txt`: `gTrainers`, `gWildMonHeaders`) |
| `tools/lc/lc.ld` | Código/dados em `0x09000000`; as variáveis do motor (~2 KB) nos 4 KB do topo do `gHeap` |
| `tools/lc/build.sh` | Compila `src/rt_*.c`, `option_menu.c`, `rt_lc_glue.c` e `rt_lc_summary.c` com `-DRT_LC=1 -mlong-calls` |
| `tools/lc/patch.py` | Coloca o blob na ROM e aplica os desvios (abaixo) |
| `src/rt_lc_glue.c` | `CB2_InitBattle` com o desvio para o motor, `InitHeap` (reserva os 4 KB), hooks de debug com trampolim |
| `src/rt_lc_summary.c` | As duas funções da tela de resumo que mostram o cooldown |

Desvios aplicados pelo `patch.py` (início de cada função substituído por `ldr r3,=destino; bx r3`): `CB2_InitBattle`, `InitHeap`, `CB2_OptionsMenuFromStartMenu`, `BufferMonMoveI`, `PokeSum_PrintMoveName`, `ItemUseCB_RareCandy`, `Task_InputHandler_Info` e `BufferMonSkills` (com trampolim); os testes de shiny (`< 8` vira `>> 10 == 0`), a remoção do TM em `Task_LearnedMove`, os scripts de item e a loja da Goldenrod são editados direto na ROM; a tabela de opções do menu da equipe ganha MOVES (cópia no blob com uma entrada a mais), com `SetPartyMonFieldSelectionActions` e `ToggleFieldMoveDescriptionWindow` passando pelo blob, e o Move Relearner volta para a equipe quando aberto dali; e o script `EventScript_PkmnCenterNurse` é reescrito (cura direta). Com `--debug`, o modo de testes é ligado e `ProcessPlayerFieldInput` passa pelos comandos de debug (lista de mapas em `src/data/rt/debug_warps_lc.h`, gerada por `tools/lc/gen_debug_warps_lc.py` direto da ROM).

As opções novas ficam nos bytes `0x90–0x92` do SaveBlock2 (sem uso no FireRed e no Liquid Crystal), porque o Liquid Crystal reescreve o byte `0x13` inteiro.
