# Relatório — Firmware AIO v2.0.0 (Nikita Marauder): status, melhorias e teto

Data: 2026-09-28 · Placa: SecureTechware 3-in-1 AIO V1.4 · Chip: ESP32-S2R2, 4MB.

---

## 1. Status — deu tudo certo?

✅ **Sim, a fundação está pronta.**
- **Toolchain 100% funcional:** consigo compilar o **ESP32 Marauder v1.17.0** (o mais
  novo oficial) para o config da AIO (`MARAUDER_FLIPPER`, ESP32-S2), com o fix de
  linker `-Wl,-zmuldefs` (a redefinição de `ieee80211_raw_frame_sanity_check` que o
  Marauder usa pra injeção crua de deauth). Core esp32 **2.0.11** (o estável).
- **Base compilada:** `esp32_marauder.ino.bin` = **1.24 MB**, usando **39% do flash**
  (de 3 MB no huge_app) e **20% da RAM** (de 320 KB). → **muita folga** para as
  nossas features.
- **Backup do firmware atual da placa** salvo (`3in1-.../backup/board-stock-v1.0.0-4MB.bin`).
- Projeto do fork em **`nikita-marauder/`** (top-level): arduino-cli + fonte v1.17.0.

Ou seja: já dá para construir e flashar firmware customizado nesta placa. A placa
está no USB do Mac agora — pronta para gravar e testar.

---

## 2. As melhorias do v2.0.0 (o que sobe de verdade)

1. **Nikita WiFi Bridge (headline).** O ESP32 sobe um **AP + servidor TCP** e injeta
   o que chega no `CommandLine::runCommand()` do Marauder, devolvendo a saída pela
   rede (captura via um `Print` "tee" que espelha o `Serial` do Marauder para o
   cliente de rede). → A Nikita, de qualquer cliente na rede, roda **QUALQUER**
   comando do Marauder sem depender do Flipper no meio. É o "acesso a tudo, sem fio".
2. **GPIO UP! / GPIO DOWN + interligação com o Flipper.** O ESP32 emite um
   **heartbeat** pela UART; o firmware do Flipper (Nikita-V8) detecta:
   - reconheceu a placa → mostra **"GPIO UP!"** na tela + evento para a Nikita;
   - placa sumiu (timeout) → **"GPIO DOWN"**.
   Isso deixa Flipper e Nikita cientes da placa em tempo real.
3. **Versão → v2.0.0** (Nikita Marauder), com marca própria no banner.
4. **Mantém 100% do Marauder v1.17.0:** todo o recon/ataque WiFi, evil portal,
   wardrive+GPS, PCAP, etc.

---

## 3. O TETO (ceiling) — honesto, medido

**Espaço (sobra muito):**
- Flash 4 MB → app hoje 39% (huge_app dá ~3 MB p/ app). Cabe a ponte WiFi + extras
  com folga (~1.9 MB livres).
- RAM 320 KB → 20% em uso; sobra para o servidor WiFi e buffers.

**Rádios da placa e quem os dirige (o teto real de capacidade):**
| Rádio | Faixa | Dirigido por | No firmware ESP32? |
|---|---|---|---|
| **ESP32-S2** | WiFi 2.4GHz | o próprio ESP32 (Marauder) | ✅ tudo de WiFi |
| **CC1101** | Sub-GHz | **o Flipper** (SPI) | ❌ (é do Flipper) |
| **nRF24** | 2.4GHz ("BLE" no marketing) | **o Flipper** (SPI) | ❌ (é do Flipper) |

⚠️ **Limite de hardware importante:** o **ESP32-S2 NÃO tem Bluetooth** (confirmado —
não há libs BT/BLE no core para o s2; o S2 é WiFi-only). Então:
- Comandos **BLE do Marauder são inertes neste chip** (blescan/blespam via ESP não
  funcionam no S2).
- O "Bluetooth/2.4GHz" da placa vem do **nRF24**, dirigido pelo Flipper — não pelo
  ESP32.
- Ter BLE no próprio ESP exigiria trocar o hardware por um ESP32-S3/original — fora
  do escopo.

**Conclusão do teto:** a placa entrega **WiFi (ESP32/Marauder) + Sub-GHz (CC1101) +
2.4GHz (nRF24)**, os dois últimos via Flipper. A Nikita cobre os três — por canais
diferentes. O v2.0.0 maximiza o eixo WiFi (bridge de rede) e a coordenação com o
Flipper; os eixos CC1101/nRF24 já são acessíveis pela Nikita via os apps do Flipper.

---

## 4. Plano de execução do v2.0.0 (cada fase testada na placa)

1. **Bridge WiFi + v2.0.0** no ESP32 → build → flash → teste (conecta na rede do AP,
   roda um comando Marauder pela rede, confere a resposta). Reversível (backup).
2. **GPIO UP!/DOWN:** heartbeat no ESP32 + detecção no Nikita-V8 (Flipper) com a
   notificação na tela + evento pra Nikita.
3. **Interligação final** com a cognição da Nikita (ela sabe do AP, do bridge e do
   estado GPIO).

**Não mexer no core (ir pro 3.x)** — perde a estabilidade de deauth (símbolos
removidos no IDF 5). Ficamos no 2.0.11.
</content>
