# RK3576 // NEURAL COMPUTE TERMINAL  (retro console + web GUI + offline RAG)

A full-screen 80s/90s-style terminal chatbot for **Rockchip RK3576** boards (Radxa ROCK 4D) that runs a
`.rkllm` model on the NPU, and can **answer from your own documents** with source references, fully offline.

* Retro UI: fixed header / telemetry / prompt / footer, scrolling conversation, 3 colour themes
* Live NPU / CPU / RAM bars, temperature, tokens per second, latency
* **RAG:** put text files in the `docs/` folders, the console indexes them (SQLite FTS5 search) and sends the
  best passages to the model. Every answer lists its sources.
* `docs/stories/` and `docs/custom/` are free folders for your own stories and files
* **Web GUI:** the same assistant in a light browser page, reachable by IP and port from your PC or phone;
  click a source to open the file with the passage highlighted
* No ncurses, no Python, no web framework: C++17 + SQLite

---

## 1. Folder layout

```
rkllm-retro-console/
├── CMakeLists.txt
├── build.sh                   helper:  ./build.sh   |   ./build.sh mock
├── README.md
├── docs/                      <<< YOUR DOCUMENTS GO HERE
│   ├── rk3576/                  chip + RKLLM notes        (samples included)
│   ├── linux/                   Linux notes               (samples included)
│   ├── cpp/                     C++ notes                 (samples included)
│   ├── drivers/                 driver notes              (samples included)
│   ├── stories/                 <<< PUT YOUR STORIES HERE (one sample story included)
│   └── custom/                  <<< ANYTHING ELSE         (sample notes included)
├── data/                      index.db is created here automatically
├── web/index.html             the web page (edit freely, no rebuild needed)
└── src/
    ├── core/                  engine wrapper, RAG (chunking, FTS5 search, prompt), telemetry, UTF-8
    ├── console/               retro terminal app
    ├── web/web_server.cpp     web server (HTTP + streaming)
    └── tools/rag_tool.cpp     index / search from the shell, no model needed
```

You can create more folders inside `docs/` at any time (e.g. `docs/manuals/`). Everything below `docs/` is indexed,
except files and folders whose name starts with `_` or `.`.

---

## 2. Install (once)

On the board (Debian / Ubuntu based image):

```bash
sudo apt update
sudo apt install -y build-essential cmake git libsqlite3-dev
```

You also need the RKLLM SDK that you already used for `llm_demo` (for `rkllm.h` and `librkllmrt.so`):

```bash
cd ~/llm
git clone https://github.com/airockchip/rknn-llm.git     # skip if you already have it
find ~ -name rkllm.h -o -name librkllmrt.so 2>/dev/null  # shows where the two files are
```

The files should be at `rknn-llm/rkllm-runtime/Linux/librkllm_api/include/rkllm.h` and
`.../aarch64/librkllmrt.so`.

## 3. Compile

Unzip the project, then build:

```bash
cd ~/llm
unzip rkllm-retro-console.zip
cd rkllm-retro-console

export RKLLM_ROOT=$HOME/llm/rknn-llm/rkllm-runtime/Linux/librkllm_api

mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DRKLLM_ROOT=$RKLLM_ROOT
make -j4
```

Or in one step: `./build.sh` (reads `RKLLM_ROOT` the same way).

Result: `build/rkllm_console` (retro terminal app), `build/rkllm_web` (web GUI server) and `build/rag_tool` (index helper).

> The library folder is baked into the binary, so `librkllmrt.so` is found even under `sudo`.

**Preview without a board or the SDK** (fake model, any Linux PC):

```bash
sudo apt install -y build-essential cmake libsqlite3-dev
./build.sh mock
./build-mock/rkllm_console anything.rkllm --fast-boot
```

## 4. Run

```bash
cd ~/llm/rkllm-retro-console
sudo ./build/rkllm_console ~/llm/models/Qwen2-0.5B-Instruct_RK3576_w4a16_g128.rkllm 512 2048
```

Arguments: model file, max new tokens, max context length (same meaning as `llm_demo`).
`sudo` is only needed to show the NPU load bar (`/sys/kernel/debug/rknpu/load`); without it the bar shows N/A.

On start, the boot screen shows `DOCUMENT INDEX ... 8 FILES / 32 CHUNKS`: the `docs/` folder was scanned
automatically. Then simply ask a question.

Optional, for stable NPU speed:

```bash
ulimit -n 102400
sudo bash ~/llm/rknn-llm/scripts/fix_freq_rk3576.sh
```

### Web GUI

```bash
cd ~/llm/rkllm-retro-console
sudo ./build/rkllm_web ~/llm/models/Qwen2-0.5B-Instruct_RK3576_w4a16_g128.rkllm 512 2048 --port 8080
```

It prints the addresses, for example `http://192.168.1.50:8080`. Open that in a browser on any device on the same
network (laptop, phone). Find the board's address any time with `hostname -I`.

* Light page: chat in the middle, document list on the left, NPU / CPU / RAM / temperature at the top.
* Tick **Answer from my documents** to use RAG. Each answer ends with a **SOURCES** box; click a source and the file
  opens with the exact passage that was given to the model highlighted. Click any file in the left list to read it.
* **Re-index documents** rescans `docs/` (after you copy new files in). **Stop** cancels an answer. **Clear chat**
  also resets the model memory.
* One answer runs at a time (one NPU). A second person asking at the same moment gets a "busy" message.
* To keep it running after you close the SSH window: `sudo nohup ./build/rkllm_web MODEL 512 2048 > web.log 2>&1 &`
  and stop it with `sudo pkill rkllm_web`. Do not run the console and the web server at the same time.
* Options: `--port N`, `--host 127.0.0.1` (this board only), `--docs`, `--db`, `--rag-k`, `--rag-bytes`, `--no-rag`.
* **There is no password.** Anyone on your network can use it and read the documents in `docs/`. Use it only on a
  network you trust, and do not forward the port to the internet.
* Wi-Fi off demo: connect your laptop to the board with an Ethernet cable (or a hotspot the board joins) and open
  the board's IP. Nothing needs internet; the page uses no external files.

---

## 5. How to use RAG

1. Copy your files into a folder under `docs/`, for example your stories:

   ```bash
   cp ~/my_stories/*.txt ~/llm/rkllm-retro-console/docs/stories/
   ```
2. In the console type `/ingest` (or restart). Only new / changed / deleted files are processed.
3. Ask questions. With RAG on, the console searches the documents, sends the best 3 passages to the model, and
   prints the sources under the answer:

   ```
   OPERATOR > What did Pip put in the lamp?
   QWEN > ...answer...
   SYS > SOURCES
         [1] stories/the_lighthouse_robot.md > The Lighthouse Robot > Chapter 2: The Storm
   ```

Good to know:

* **Formats:** `.md .txt .rst .c .h .cc .cpp .hpp .dts .dtsi .py .sh .cfg .conf .ini .yaml .yml .log`.
  For PDFs convert on a PC first: `pdftotext -layout manual.pdf manual.txt`, then copy the `.txt`.
* **Headings:** in `.md` / `.rst` files, headings (`# Title`, `## Chapter 2`) are stored with each passage and shown
  in the sources. Use them in your stories.
* **Search is keyword based** (SQLite FTS5 / BM25). Ask with words that appear in the text:
  *"how do I scan the I2C bus"* finds the `i2cdetect` note, *"how do I make the board faster"* may find nothing if the
  notes say "increase CPU frequency".
* **No match = no model call.** If nothing relevant is found, the console tells you instead of letting the small model
  guess. Use `/rag off` for normal chat.
* **Each RAG question is independent** (history is not kept, because the documents are re-sent every time).
* **A 0.5B model is weak** at following the context. It can ignore passages or make things up. The SOURCES list is
  produced by the program, not the model, so it is always correct. Use `/search words` to see the raw passages.
  For better answers use a larger `.rkllm` model (1.5B or 3B).
* **Chinese / Japanese text** in documents is indexed, but each unbroken run of CJK characters is one search word,
  so search works best with English keywords or whole words.
* **Sources are always shown, even when the model's answer is poor.** A 0.5B model sometimes says "the context does not
  contain..." even though the passage is there. Open the source to check, ask with the words used in your files, or
  try a bigger model.
* **Context size:** the prompt carries up to 3200 bytes of document text (about 800 tokens of English). Keep
  `max_context_len` at 2048 or more. Tune with `--rag-k` and `--rag-bytes`.

### Test the search without the model

```bash
./build/rag_tool ingest docs data/index.db
./build/rag_tool stats  data/index.db
./build/rag_tool search data/index.db "how do I scan the I2C bus" 3
```

---

## 6. Keys and commands

| Key | Action |
|---|---|
| `Enter` | send |
| `↑` / `↓` | scroll conversation |
| `PgUp` / `PgDn` | scroll a page |
| `Home` / `End` | oldest message / latest (resumes auto-follow) |
| `Ctrl+C` | stop an answer; clears typed text; quits on an empty prompt |
| `Ctrl+L` | clear screen and model memory |
| `Ctrl+T` | change theme |
| `Ctrl+P` / `Ctrl+N` | previous / next sent message |
| `←` `→` `Ctrl+A` `Ctrl+E` `Ctrl+U` `Del` | edit the input line |

| Command | Action |
|---|---|
| `/rag on` / `/rag off` | answer from your documents, or plain chat |
| `/ingest` | rescan the `docs/` folder |
| `/docs` | what is indexed, per folder |
| `/search words` | show matching passages without asking the model |
| `/memory on` / `/memory off` | chat history in plain-chat mode |
| `/theme green\|amber\|ice` | colours |
| `/clear`, `/help`, `/exit` | |

Scrolling: the conversation follows new output while you are at the bottom; scroll up and it stays put
(`PAUSED -N` in the header); `End` resumes. Header, telemetry, prompt and footer never scroll.

## 7. Options

```
rkllm_console MODEL.rkllm [max_new_tokens=512] [max_context_len=2048] [options]

  --theme NAME         green (default) | amber | ice
  --board TEXT / --soc TEXT / --bot-name TEXT / --model-label TEXT   banner texts
  --top-k N (20)  --top-p X (0.9)  --temp X (0.7)  --repeat-penalty X (1.1)
  --no-history         plain chat starts with memory off
  --mouse              mouse-wheel scrolling (hold Shift to select text)
  --no-bg              keep your terminal background
  --fast-boot          skip boot delays
  --log FILE           where RKLLM's own messages go (default /tmp/rkllm_console.log)

  --docs DIR           documents folder   (default: <project>/docs)
  --db FILE            index file         (default: <project>/data/index.db)
  --rag-k N            passages per question (3)
  --rag-bytes N        max document bytes per question (3200)
  --no-rag             turn document search off
```

Example: `sudo ./build/rkllm_console MODEL.rkllm 512 2048 --theme amber --docs ~/my_docs`

## 8. Troubleshooting

* **`SQLite3 development files not found`:** `sudo apt install -y libsqlite3-dev`
* **`Could not find the RKLLM runtime`:** pass `-DRKLLM_ROOT=...` (see section 3).
* **RAG shows `UNAVAILABLE` / cannot open index:** the `data/` folder is owned by another user. If you ran once with
  `sudo` and later without, fix it with `sudo chown -R $USER: data` (or always use the same way of starting).
* **Index seems stale or broken:** delete it, it is rebuilt on start: `rm -f data/index.db*`
* **A file is not found by search:** check its extension (section 5), that its name does not start with `_`,
  that it is under `docs/`, and that you ran `/ingest`. `/docs` shows what is indexed.
* **NPU shows N/A:** run with `sudo`.
* **Web page does not open from another device:** check `hostname -I`, that both are on the same network, that the
  server was not started with `--host 127.0.0.1`, and that no firewall blocks the port (`sudo ufw allow 8080` if ufw is used).
* **`bind/listen: Address already in use`:** another copy is running (`sudo pkill rkllm_web`) or use `--port 8081`.
* **Context full error:** `/clear`, or start with a larger `max_context_len` (the model file reported 4096 max).
* **Runtime messages** (`I rkllm: ...`) go to `/tmp/rkllm_console.log`; if the model fails to load, the last lines are shown.
* **Terminal too small:** needs at least 60x20; 80x24 or larger looks best. Garbled terminal after a crash: `reset`.
* **Colours wrong:** use a 256-colour terminal (`echo $TERM` -> `xterm-256color`).


License: This project is available for educational and non-commercial use under the Embedded AI Lab Educational Use License. Commercial use requires prior written permission. Please see the LICENSE file for the full terms.

Contact Info : embeddedailab.official@gmail.com
