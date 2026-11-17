#include "host/settings.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <format>
#include <span>

namespace {
struct Control {
  const char* id;
  const char* label;  // what it does in the game: the manual's "Controls" page, with the default setup
  const char* ps2;    // the button, as the game's own texts call it
  const char* labelEn;
  const char* ps2En;
};
constexpr Control kControlNames[Settings::kControls] = {
    {"select", "Abrir o mapa", "Select", "Open the map", "Select"},
    {"l3", "Correr", "L3", "Sprint", "L3"},
    {"r3", "Sem função no jogo", "R3", "Not used by the game", "R3"},
    {"start", "Abrir o menu", "Start", "Open the menu", "Start"},
    {"cima", "IA do Innocent Devil: cima", "Direcional", "Innocent Devil AI: up", "D-pad"},
    {"direita", "IA do Innocent Devil: direita", "Direcional", "Innocent Devil AI: right", "D-pad"},
    {"baixo", "IA do Innocent Devil: baixo", "Direcional", "Innocent Devil AI: down", "D-pad"},
    {"esquerda", "IA do Innocent Devil: esquerda", "Direcional", "Innocent Devil AI: left", "D-pad"},
    {"l2", "Trocar de alvo", "L2", "Change target", "L2"},
    {"r2", "Travar no alvo", "R2", "Lock on", "R2"},
    {"l1", "Centralizar a câmera", "L1", "Reset the camera", "L1"},
    {"r1", "Defender", "R1", "Guard", "R1"},
    {"triangulo", "Habilidade do Innocent Devil / Voltar", "Triângulo", "Innocent Devil ability / Back", "Triangle"},
    {"bola", "Golpe final / Roubar / Examinar / Mudar de área", "Bola", "Final attack / Steal / Examine / Change area", "Circle"},
    {"x", "Pular / Confirmar", "X (cruz)", "Jump / Confirm", "Cross"},
    {"quadrado", "Atacar", "Quadrado", "Attack", "Square"},
    {"analogico_esq_cima", "Andar: para cima", "Analógico esq.", "Move: up", "Left stick"},
    {"analogico_esq_baixo", "Andar: para baixo", "Analógico esq.", "Move: down", "Left stick"},
    {"analogico_esq_esquerda", "Andar: para a esquerda", "Analógico esq.", "Move: left", "Left stick"},
    {"analogico_esq_direita", "Andar: para a direita", "Analógico esq.", "Move: right", "Left stick"},
    {"analogico_dir_cima", "Câmera: para cima", "Analógico dir.", "Camera: up", "Right stick"},
    {"analogico_dir_baixo", "Câmera: para baixo", "Analógico dir.", "Camera: down", "Right stick"},
    {"analogico_dir_esquerda", "Câmera: para a esquerda", "Analógico dir.", "Camera: left", "Right stick"},
    {"analogico_dir_direita", "Câmera: para a direita", "Analógico dir.", "Camera: right", "Right stick"},
};

struct Named {
  int code;
  const char* name;
};
constexpr Named kKeyNames[] = {
    {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},        {VK_RETURN, "Enter"},     {VK_SHIFT, "Shift"},
    {VK_CONTROL, "Ctrl"},   {VK_MENU, "Alt"},       {VK_ESCAPE, "Esc"},       {VK_SPACE, "Espaco"},
    {VK_PRIOR, "PageUp"},   {VK_NEXT, "PageDown"},  {VK_END, "End"},          {VK_HOME, "Home"},
    {VK_LEFT, "Esquerda"},  {VK_UP, "Cima"},        {VK_RIGHT, "Direita"},    {VK_DOWN, "Baixo"},
    {VK_INSERT, "Insert"},  {VK_DELETE, "Delete"},  {VK_CAPITAL, "CapsLock"}, {VK_OEM_1, "Ç"},
    {VK_OEM_PLUS, "="},     {VK_OEM_COMMA, ","},    {VK_OEM_MINUS, "-"},      {VK_OEM_PERIOD, "."},
    {VK_OEM_2, ";"},        {VK_OEM_3, "'"},        {VK_OEM_4, "´"},          {VK_OEM_5, "]"},
    {VK_OEM_6, "["},        {VK_OEM_7, "~"},        {VK_LSHIFT, "ShiftEsq"},  {VK_RSHIFT, "ShiftDir"},
    {VK_LCONTROL, "CtrlEsq"}, {VK_RCONTROL, "CtrlDir"},
    {VK_LBUTTON, "MouseEsq"}, {VK_RBUTTON, "MouseDir"}, {VK_MBUTTON, "MouseMeio"},
};
constexpr Named kPadNames[] = {
    {XINPUT_GAMEPAD_A, "A"},           {XINPUT_GAMEPAD_B, "B"},          {XINPUT_GAMEPAD_X, "X"},
    {XINPUT_GAMEPAD_Y, "Y"},           {XINPUT_GAMEPAD_LEFT_SHOULDER, "LB"}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB"},
    {Settings::kLeftTrigger, "LT"},    {Settings::kRightTrigger, "RT"},  {XINPUT_GAMEPAD_BACK, "Back"},
    {XINPUT_GAMEPAD_START, "Start"},   {XINPUT_GAMEPAD_LEFT_THUMB, "LS"}, {XINPUT_GAMEPAD_RIGHT_THUMB, "RS"},
    {XINPUT_GAMEPAD_DPAD_UP, "Cima"},  {XINPUT_GAMEPAD_DPAD_DOWN, "Baixo"}, {XINPUT_GAMEPAD_DPAD_LEFT, "Esquerda"},
    {XINPUT_GAMEPAD_DPAD_RIGHT, "Direita"},
};

std::string trim(std::string s) {
  const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
  s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), space));
  s.erase(std::find_if_not(s.rbegin(), s.rend(), space).base(), s.end());
  return s;
}
}  // namespace

const char* Settings::id(int control) { return kControlNames[control].id; }
const char* Settings::label(int control) { return english() ? kControlNames[control].labelEn : kControlNames[control].label; }
const char* Settings::ps2Name(int control) { return english() ? kControlNames[control].ps2En : kControlNames[control].ps2; }

bool english() { return settings().language == "en"; }

std::string keyLabel(int vk) {
  // As shown: the file's names, in the window's language.
  static constexpr std::pair<int, const char*> kPt[] = {
      {VK_SPACE, "Espaço"}, {VK_LSHIFT, "Shift esq."}, {VK_RSHIFT, "Shift dir."}, {VK_LCONTROL, "Ctrl esq."}, {VK_RCONTROL, "Ctrl dir."},
      {VK_LBUTTON, "Clique esq."}, {VK_RBUTTON, "Clique dir."}, {VK_MBUTTON, "Clique do meio"}};
  static constexpr std::pair<int, const char*> kEn[] = {
      {VK_SPACE, "Space"}, {VK_LEFT, "Left"}, {VK_UP, "Up"}, {VK_RIGHT, "Right"}, {VK_DOWN, "Down"},
      {VK_LSHIFT, "Left Shift"}, {VK_RSHIFT, "Right Shift"}, {VK_LCONTROL, "Left Ctrl"}, {VK_RCONTROL, "Right Ctrl"},
      {VK_LBUTTON, "Left click"}, {VK_RBUTTON, "Right click"}, {VK_MBUTTON, "Middle click"}};
  for (const auto& [code, name] : english() ? std::span<const std::pair<int, const char*>>(kEn) : std::span<const std::pair<int, const char*>>(kPt))
    if (code == vk) return name;
  const std::string name = keyName(vk);
  return english() && name.rfind("Tecla", 0) == 0 ? "Key" + name.substr(5) : name;
}

std::string padLabel(uint16_t input) {
  if (english()) {
    if (input == XINPUT_GAMEPAD_DPAD_UP) return "Up";
    if (input == XINPUT_GAMEPAD_DPAD_DOWN) return "Down";
    if (input == XINPUT_GAMEPAD_DPAD_LEFT) return "Left";
    if (input == XINPUT_GAMEPAD_DPAD_RIGHT) return "Right";
  }
  return padName(input);
}

std::string keyName(int vk) {
  if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, char(vk));
  if (vk >= VK_F1 && vk <= VK_F24) return std::format("F{}", vk - VK_F1 + 1);
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return std::format("Num{}", vk - VK_NUMPAD0);
  for (const Named& k : kKeyNames)
    if (k.code == vk) return k.name;
  return vk ? std::format("Tecla{}", vk) : "";
}

int keyFromName(const std::string& name) {
  if (name.empty()) return 0;
  if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0]))) return std::toupper(static_cast<unsigned char>(name[0]));
  for (int vk = 1; vk < 256; ++vk)
    if (keyName(vk) == name) return vk;
  return 0;
}

std::string padName(uint16_t input) {
  for (const Named& p : kPadNames)
    if (p.code == input) return p.name;
  return "";
}

uint16_t padFromName(const std::string& name) {
  for (const Named& p : kPadNames)
    if (name == p.name) return uint16_t(p.code);
  return 0;
}

Settings Settings::defaults() {
  Settings s;
  s.language = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE ? "pt" : "en";
  // Keyboard and mouse, for a hand on W/A/S/D and the other on the mouse: W/A/S/D = left stick
  // (Hector moves with it), the mouse turns the camera (and I/J/K/L = right stick), left and right
  // buttons = square and circle (the attack and the final attack), Space = cross, E = triangle,
  // F = R1 (guard), Q = L1, 1 = L2, middle button = R2 (lock on), Shift = L3 (the sprint), Tab =
  // SELECT (the map), Enter = START, arrows = D-pad (the Innocent Devil's AI, and menus).
  s.keys = {VK_TAB, VK_SHIFT, 0,   VK_RETURN, VK_UP, VK_RIGHT, VK_DOWN, VK_LEFT, '1', VK_MBUTTON, 'Q', 'F',
            'E',    VK_RBUTTON, VK_SPACE, VK_LBUTTON, 'W', 'S', 'A', 'D', 'I', 'K', 'J', 'L'};
  // Xbox layout onto the DualShock: A = cross, B = circle, X = square, Y = triangle.
  s.pad = {XINPUT_GAMEPAD_BACK,      XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_START,
           XINPUT_GAMEPAD_DPAD_UP,   XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN,   XINPUT_GAMEPAD_DPAD_LEFT,
           kLeftTrigger,             kRightTrigger,             XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
           XINPUT_GAMEPAD_Y,         XINPUT_GAMEPAD_B,          XINPUT_GAMEPAD_A,           XINPUT_GAMEPAD_X};
  return s;
}

void Settings::load(const std::filesystem::path& from) {
  file = from;
  std::ifstream in(from);
  std::string line, section;
  while (std::getline(in, line)) {
    if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);  // saved by an editor with a UTF-8 mark
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    if (section == "disco" && key == "iso") {
      iso = value;
      continue;
    }
    if (section == "disco" && key == "limitar_compilacoes") {
      limitBuildJobs = value != "0";
      continue;
    }
    if (section == "janela") {
      if (key == "tela_cheia") fullscreen = value == "1";
      if (key == "tamanho_janela") {
        int w = 0, h = 0;
        if (std::sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) windowWidth = w, windowHeight = h;
      }
      if (key == "posicao_janela") {
        int x = 0, y = 0;
        if (std::sscanf(value.c_str(), "%d,%d", &x, &y) == 2) windowX = x, windowY = y;
      }
      if (key == "proporcao") wide = value == "16:9" || value == "ultrawide", ultrawide = value == "ultrawide";
      if (key == "resolucao") scale = std::clamp(std::atoi(value.c_str()), 1, 6);
      if (key == "mostrar_fps") showFps = value == "1";
      if (key == "brilho") brightness = std::clamp(std::atoi(value.c_str()) / 10 * 10, 50, 200);
      if (key == "volume") volume = std::clamp(std::atoi(value.c_str()) / 10 * 10, 0, 100);
      if (key == "interpolacao") interpolation = value == "1";
      if (key == "fxaa") fxaa = value == "1";
      if (key == "taxa_variavel") variableRefresh = value == "1";
      if (key == "tamanho_hud") hudScale = std::clamp(std::atoi(value.c_str()), 80, 125);
      if (key == "baixa_latencia") lowLatency = value == "1";
      if (key == "nitidez") sharpness = std::clamp(std::atoi(value.c_str()) / 10 * 10, 0, 100);
      if (key == "filtro_crt") crt = value == "1";
      if (key == "daltonismo") colourBlind = std::clamp(std::atoi(value.c_str()), 0, 3);
      if (key == "idioma" && (value == "pt" || value == "en")) language = value;
      if (key == "avisar_versao_nova") checkUpdates = value == "1";
      continue;
    }
    if (section == "jogo") {
      if (key == "ataque_no_alvo") attackTurn = value == "1";
      if (key == "vibracao") vibration = value == "1";
      if (key == "icones_xbox") xboxIcons = value == "1";
      if (key == "corrida") sprint = value == "1";
      if (key == "filtro_textura") textureFilter = value == "1";
      if (key == "filtro_anisotropico") anisotropy = std::clamp(std::atoi(value.c_str()), 0, 16);
      if (key == "carregamento_rapido") fastLoading = value == "1";
      if (key == "abertura_rapida") fastOpening = value == "1";
      if (key == "videos_nativos") nativeMovies = value == "1";
      if (key == "minimapa") minimap = value == "1";
      if (key == "pacote_texturas") texturePack = value == "1";
      if (key == "salvar_texturas") dumpTextures = value == "1";
      continue;
    }
    if (section == "mouse") {
      if (key == "camera_com_mouse") mouseCamera = value == "1";
      if (key == "sensibilidade") mouseSensitivity = std::clamp(std::atoi(value.c_str()), 1, 10);
      if (key == "inverter_vertical") invertMouseY = value == "1";
      if (key == "inverter_horizontal") invertMouseX = value == "1";
      continue;
    }
    if (section == "som") {
      if (key == "dispositivo") audioDevice = value;
      if (key == "surround_51") surround = value == "1";
      continue;
    }
    if (section == "extras") {
      if (key == "cronometro") speedrunTimer = value == "1";
      if (key == "livesplit") liveSplit = value == "1";
      if (key == "avisos_conquistas") achievementNotices = value == "1";
      if (key == "discord") discord = value == "1";
      if (key == "discord_id_aplicativo" && !value.empty()) discordClientId = value;
      continue;
    }
    if (section == "controle_ajustes") {
      if (key == "zona_morta") stickDeadZone = std::clamp(std::atoi(value.c_str()), 0, 50);
      if (key == "velocidade_camera") stickCameraSpeed = std::clamp(std::atoi(value.c_str()), 50, 300);
      if (key == "inverter_camera_horizontal") invertStickX = value == "1";
      if (key == "inverter_camera_vertical") invertStickY = value == "1";
      continue;
    }
    for (int c = 0; c < kControls; ++c) {
      if (key != id(c)) continue;
      if (section == "teclado") keys[c] = uint8_t(keyFromName(value));
      if (section == "controle" && c < kButtons) pad[c] = padFromName(value);
    }
  }
}

void Settings::save() const {
  if (file.empty()) return;
  std::ofstream out(file);
  // The comments in the language of the options (the names and values stay the same in both).
  const bool en = language == "en";
  auto note = [en](const char* pt, const char* english) { return std::string("# ") + (en ? english : pt) + "\n"; };
  out << note("Configuracoes do port. Tudo isto tambem muda com F1 dentro do jogo.",
              "The port's settings. All of this also changes with F1 in the game.")
      << note("Teclado: letras, numeros, F1-F24, Num0-Num9, Enter, Espaco, Backspace, Tab, Esc, Shift, Ctrl,",
              "Keyboard: letters, digits, F1-F24, Num0-Num9, Enter, Espaco (space), Backspace, Tab, Esc, Shift, Ctrl,")
      << note("Alt, Cima, Baixo, Esquerda, Direita... Controle (Xbox): A, B, X, Y, LB, RB, LT, RT, Back, Start,",
              "Alt, Cima (up), Baixo (down), Esquerda (left), Direita (right)... Controller (Xbox): A, B, X, Y, LB, RB,")
      << note("LS, RS, Cima, Baixo, Esquerda, Direita. Vazio = nenhuma.", "LT, RT, Back, Start, LS, RS, Cima, Baixo, Esquerda, Direita. Empty = none.") << "\n";
  out << "[disco]\n"
      << note("A imagem do disco do jogo (SLES-53755); vazio = o primeiro .iso desta pasta ou de uma acima",
              "The game's disc image (SLES-53755); empty = the first .iso in this folder or one above it")
      << "iso=" << iso << "\n"
      << note("Setup: compilar so quantos arquivos de cada vez a memoria livre comporta (0 = um por processador)",
              "Setup: compile only as many files at once as the free memory holds (0 = one per processor)")
      << "limitar_compilacoes=" << (limitBuildJobs ? 1 : 0) << "\n\n";
  out << "[janela]\n"
      << note("Ao abrir o jogo, pergunta ao GitHub se saiu uma versao nova do port (e avisa no titulo e no F1)",
              "When the game opens, asks GitHub whether a new version of the port is out (and says so in the title and in F1)")
      << "avisar_versao_nova=" << (checkUpdates ? 1 : 0) << "\ntela_cheia=" << (fullscreen ? 1 : 0) << "\n"
      << note("Tamanho da janela fora da tela cheia, em pixels (em tela cheia vale o do monitor)",
              "Size of the window out of full screen, in pixels (in full screen, the monitor's)")
      << "tamanho_janela=" << windowWidth << 'x' << windowHeight << "\n"
      << note("Onde a janela estava (canto superior esquerdo, em pixels da area de trabalho); vazio = no meio da tela",
              "Where the window was (its top left corner, in desktop pixels); empty = in the middle of the screen")
      << "posicao_janela=" << (windowX == kNoPlace ? std::string() : std::to_string(windowX) + "," + std::to_string(windowY)) << "\n"
      << note("Proporcao: 4:3, 16:9 ou ultrawide (tao larga quanto a tela, 21:9 ou 32:9)",
              "Proportion: 4:3, 16:9 or ultrawide (as wide as the screen, 21:9 or 32:9)")
      << "proporcao=" << (!wide ? "4:3" : ultrawide ? "ultrawide" : "16:9") << "\n"
      << note("Resolucao do desenho: 1 = a do PS2; 2 a 6 = tantas vezes mais nitida (pesa mais na placa de",
              "Drawing resolution: 1 = the PS2's; 2 to 6 = that many times sharper (harder on the graphics card;")
      << note("video; o 6x usa uns 1,5 GB de memoria de video nas cenas mais pesadas)", "6x takes about 1.5 GB of video memory in the heaviest scenes)")
      << "resolucao=" << scale << "\n"
      << note("Quadros por segundo no canto superior direito (F9 liga e desliga)", "Frames per second in the top right corner (F9 turns it on and off)")
      << "mostrar_fps=" << (showFps ? 1 : 0) << "\n"
      << note("Brilho da imagem em porcento (100 = como o jogo desenha; mais claro acima, mais escuro abaixo)",
              "Brightness of the picture in percent (100 = as the game draws it; brighter above, darker below)")
      << "brilho=" << brightness << "\n"
      << note("Volume do som em porcento", "Sound volume in percent") << "volume=" << volume << "\n"
      << note("Interpolacao: quadros feitos entre os do jogo, em telas de 100 Hz ou mais (atrasa a imagem um pouco)",
              "Interpolation: frames made between the game's, on 100 Hz or faster screens (delays the picture a little)")
      << "interpolacao=" << (interpolation ? 1 : 0) << "\n"
      << note("Anti-aliasing FXAA: suaviza os serrilhados da imagem", "FXAA anti-aliasing: smooths the picture's jagged edges")
      << "fxaa=" << (fxaa ? 1 : 0) << "\n"
      << note("Nitidez em porcento (0 = desligada)", "Sharpening in percent (0 = off)") << "nitidez=" << sharpness << "\n"
      << note("Filtro de TV de tubo (linhas e mascara)", "CRT TV filter (scanlines and mask)") << "filtro_crt=" << (crt ? 1 : 0) << "\n"
      << note("Cores para daltonismo: 0 = nenhum, 1 = protanopia, 2 = deuteranopia, 3 = tritanopia",
              "Colours for colour blindness: 0 = none, 1 = protanopia, 2 = deuteranopia, 3 = tritanopia")
      << "daltonismo=" << colourBlind << "\n"
      << note("Monitor com G-Sync ou FreeSync: cada imagem aparece assim que fica pronta (em outros monitores rasga)",
              "G-Sync or FreeSync monitor: each picture shows as soon as it is ready (on other monitors it tears)")
      << "taxa_variavel=" << (variableRefresh ? 1 : 0) << "\n"
      << note("Baixa latencia: os controles sao lidos logo antes de cada quadro do jogo", "Low latency: the controls are read just before each of the game's frames")
      << "baixa_latencia=" << (lowLatency ? 1 : 0) << "\n"
      << note("Tamanho da HUD durante o jogo em porcento (80 a 125; cada parte cresce a partir do seu canto)",
              "Size of the HUD during play in percent (80 to 125; each piece grows from its corner)")
      << "tamanho_hud=" << hudScale << "\n"
      << "# Idioma das opcoes e controles (F1): pt ou en / Language of the options and controls (F1): pt or en\nidioma=" << language
      << "\n\n[jogo]\n"
      << note("Com um inimigo travado, os ataques viram para ele", "With an enemy locked on, attacks turn to it")
      << "ataque_no_alvo=" << (attackTurn ? 1 : 0) << "\n"
      << note("O controle vibra quando o jogo faz o DualShock 2 vibrar", "The controller vibrates when the game makes the DualShock 2 vibrate")
      << "vibracao=" << (vibration ? 1 : 0) << "\n"
      << note("Icones de botao do Xbox (A, B, X, Y, LB...) no lugar dos do PS2 nos avisos do jogo",
              "Xbox button icons (A, B, X, Y, LB...) in place of the PS2's in the game's prompts")
      << "icones_xbox=" << (xboxIcons ? 1 : 0) << "\n"
      << note("L3 (apertar o analogico esquerdo) liga e desliga a corrida mais rapida", "L3 (pressing the left stick) turns the faster run on and off")
      << "corrida=" << (sprint ? 1 : 0) << "\n"
      << note("Texturas suavizadas como no PS2; 0 = pixels duros", "Textures smoothed as on the PS2; 0 = hard pixels")
      << "filtro_textura=" << (textureFilter ? 1 : 0) << "\n"
      << note("Filtro anisotropico: 2, 4, 8 ou 16 (o chao e as paredes em angulo cintilam menos de longe); 0 = como o PS2",
              "Anisotropic filter: 2, 4, 8 or 16 (floors and walls at an angle shimmer less far away); 0 = as the PS2")
      << "filtro_anisotropico=" << anisotropy << "\n"
      << note("Portas e telas de Loading sem esperar o tempo real; a musica e o som da porta somem",
              "Doors and Loading screens without waiting the real time; the music and the door's sound go")
      << "carregamento_rapido=" << (fastLoading ? 1 : 0) << "\n"
      << note("Abertura sem esperar: a checagem do cartao de memoria e o logo da KONAMI passam na hora",
              "Opening without waiting: the memory card check and the KONAMI logo go by at once")
      << "abertura_rapida=" << (fastOpening ? 1 : 0) << "\n"
      << note("Videos decodificados direto no PC em vez de pelo IPU emulado: bem mais leves para processadores antigos (vale a partir do proximo video)",
              "Movies decoded straight on the PC instead of through the emulated IPU: far lighter on old processors (applies from the next movie)")
      << "videos_nativos=" << (nativeMovies ? 1 : 0) << "\n"
      << note("Mapa no canto da tela (F8 liga e desliga)", "Map in the corner of the screen (F8 turns it on and off)") << "minimapa=" << (minimap ? 1 : 0) << "\n"
      << note("Pacote de texturas: as imagens de textures\\replace no lugar das do jogo (vale ao abrir o jogo)",
              "Texture pack: the pictures in textures\\replace in place of the game's (applies when the game opens)")
      << "pacote_texturas=" << (texturePack ? 1 : 0) << "\n"
      << note("Salva as texturas do jogo em textures\\dump, para fazer um pacote (vale ao abrir o jogo)",
              "Saves the game's textures in textures\\dump, to make a pack (applies when the game opens)")
      << "salvar_texturas=" << (dumpTextures ? 1 : 0) << "\n\n[mouse]\n"
      << note("O mouse gira a camera (o analogico direito); Esc solta o cursor, um clique na janela prende de novo",
              "The mouse turns the camera (the right stick); Esc lets the cursor go, a click in the window holds it again")
      << "camera_com_mouse=" << (mouseCamera ? 1 : 0) << "\n"
      << note("De 1 a 10", "From 1 to 10") << "sensibilidade=" << mouseSensitivity << "\n"
      << note("1 = empurrar o mouse para a frente olha para baixo", "1 = pushing the mouse forward looks down") << "inverter_vertical=" << (invertMouseY ? 1 : 0) << "\n"
      << note("1 = mover o mouse para a direita gira a camera para a esquerda", "1 = moving the mouse right turns the camera left")
      << "inverter_horizontal=" << (invertMouseX ? 1 : 0) << "\n\n[controle_ajustes]\n"
      << note("Zona morta dos analogicos em porcento (0 = a do XInput, 24 e 27)", "Dead zone of the sticks in percent (0 = XInput's, 24 and 27)")
      << "zona_morta=" << stickDeadZone << "\n"
      << note("Velocidade da camera no analogico direito, em porcento da do jogo (acima de 100, mais rapida que o jogo gira)",
              "Camera speed on the right stick, in percent of the game's (above 100, faster than the game turns)")
      << "velocidade_camera=" << stickCameraSpeed << "\ninverter_camera_horizontal=" << (invertStickX ? 1 : 0)
      << "\ninverter_camera_vertical=" << (invertStickY ? 1 : 0) << "\n\n[som]\n"
      << note("Saida de som pelo nome (vazio = a padrao do Windows)", "Sound output by name (empty = Windows' default)") << "dispositivo=" << audioDevice << "\n"
      << note("Som 5.1: o Pro Logic II do jogo (escolha Pro Logic II nas opcoes de som dele) aberto em 6 canais",
              "5.1 sound: the game's Pro Logic II (choose Pro Logic II in its sound options) opened into 6 channels")
      << "surround_51=" << (surround ? 1 : 0) << "\n\n[extras]\n"
      << note("Cronometro de speedrun no canto (F7): o tempo do jogo desde que ele comecou, sem os carregamentos",
              "Speedrun timer in the corner (F7): the game's time since it started, without the loadings")
      << "cronometro=" << (speedrunTimer ? 1 : 0) << "\n"
      << note("O LiveSplit segue o cronometro (ligue o LiveSplit Server nele: Control > Start TCP Server, porta 16834)",
              "LiveSplit follows the timer (turn its LiveSplit Server on: Control > Start TCP Server, port 16834)")
      << "livesplit=" << (liveSplit ? 1 : 0) << "\n"
      << note("Aviso na tela quando uma conquista do port e alcancada", "A notice on the screen when one of the port's achievements is reached")
      << "avisos_conquistas=" << (achievementNotices ? 1 : 0) << "\n"
      << note("Mostrar no Discord a area e o nivel do Hector", "Show Hector's area and level on Discord") << "discord=" << (discord ? 1 : 0) << "\n"
      << note("Id do aplicativo do Discord (vazio = o do port)", "Discord application id (empty = the port's)")
      << "discord_id_aplicativo=" << (discordClientId == kDiscordClientId ? "" : discordClientId) << "\n\n[teclado]\n";
  for (int c = 0; c < kControls; ++c) out << id(c) << '=' << keyName(keys[c]) << '\n';
  out << "\n[controle]\n";
  for (int c = 0; c < kButtons; ++c) out << id(c) << '=' << padName(pad[c]) << '\n';
}

Settings& settings() {
  static Settings s = Settings::defaults();
  return s;
}
