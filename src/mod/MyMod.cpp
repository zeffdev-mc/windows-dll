#include "mod/MyMod.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h" // este header nao tem pragma once: inclua so aqui
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/input/RectangleArea.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

namespace my_mod {

namespace {

// ---------------------------------------------------------------------------
// ESP 3D: caixa em arame (12 arestas) ao redor de cada jogador, em ciano claro.
//
// Teclas em jogo:
//   R      liga/desliga o ESP
//   N      alterna entre caixa 3D (arame) e caixa 2D simples
//   B      troca a fonte do FOV: FOV real do jogo  <->  FOV da matriz da camera
//   [  ]   diminui / aumenta a escala geral (5% por toque)
// O log do LeviLamina mostra, ao ligar e a cada ajuste, os valores usados.
// ---------------------------------------------------------------------------

constexpr int kKeyScaleDown = 219; // [
constexpr int kKeyScaleUp   = 221; // ]

bool  gActive       = false;
bool  gEspOn        = false;
bool  gLogNextFrame = false;
bool  gMode3D       = true;
bool  gUseGameFov   = true;
float gScale        = 1.0f;
bool  gSeenHud      = false;
int   gPassesNoHud  = 0;
std::chrono::steady_clock::time_point gLastDraw{};

constexpr int   kRectBudget = 600;   // maximo de retangulos por quadro (evita lag)
constexpr float kDot        = 1.0f;  // tamanho de cada ponto em unidades da GUI

struct Vec3f {
    float x, y, z;
};

float dot3(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Dados da camera lidos uma vez por quadro.
struct CamData {
    Vec3f pos, fwd, rgt, up;
    float kx, ky; // fatores de escala horizontal e vertical
    float W, H;   // tamanho do espaco de desenho da UI
};

// FOV vertical real que o jogo usa no mundo (em graus). 0 = indisponivel.
float gameFovDegrees(ClientInstance& ci) {
    LevelRenderer* lr = ci.getLevelRenderer();
    if (!lr) {
        return 0.0f;
    }
    LevelRendererPlayer* lrp = ci.getLevelRendererPlayer();
    if (!lrp) {
        return 0.0f;
    }
    float f = lrp->getFov(1.0f, true);
    if (f > 0.0f && f < 3.2f) {
        f = f * 180.0f / 3.14159265f; // veio em radianos
    }
    return (f > 5.0f && f < 175.0f) ? f : 0.0f;
}

CamData readCamera(ClientInstance& ci, float W, float H) {
    auto& cam = ci.getCamera();

    CamData c;
    c.pos = {cam.mPosition->x, cam.mPosition->y, cam.mPosition->z};
    c.fwd = {cam.mForward->x, cam.mForward->y, cam.mForward->z};
    c.rgt = {cam.mRight->x, cam.mRight->y, cam.mRight->z};
    c.up  = {cam.mUp->x, cam.mUp->y, cam.mUp->z};

    float aspect = W / H;
    float p11    = std::abs(cam.getProjectionMatrix()[1][1]);
    float ky     = p11; // fator vertical vindo da matriz da camera

    if (gUseGameFov) {
        float fov = gameFovDegrees(ci);
        if (fov > 0.0f) {
            ky = 1.0f / std::tan(fov * 3.14159265f / 180.0f * 0.5f);
        }
    }

    c.ky = gScale * ky;
    c.kx = gScale * ky / aspect;
    c.W  = W;
    c.H  = H;
    return c;
}

// Projeta um ponto do mundo para coordenadas da UI. false = atras da camera.
bool project(CamData const& c, Vec3f p, float& sx, float& sy) {
    Vec3f d{p.x - c.pos.x, p.y - c.pos.y, p.z - c.pos.z};
    float z = dot3(d, c.fwd);
    if (z < 0.05f) {
        return false;
    }
    float x = dot3(d, c.rgt);
    float y = dot3(d, c.up);

    sx = (x * c.kx / z * 0.5f + 0.5f) * c.W;
    sy = (0.5f - y * c.ky / z * 0.5f) * c.H;
    return true;
}

void fillRect(MinecraftUIRenderContext& ctx, float xa, float xb, float ya, float yb, mce::Color const& color) {
    RectangleArea r;
    r._x0 = xa;
    r._x1 = xb;
    r._y0 = ya;
    r._y1 = yb;
    ctx.fillRectangle(r, color, 1.0f);
}

// Aresta feita de pontos; maxDots limita o custo (aresta longa fica pontilhada em vez de pesada).
void drawEdge(
    MinecraftUIRenderContext& ctx,
    float ax, float ay, float bx, float by,
    int maxDots, float W, float H,
    mce::Color const& color
) {
    float dx  = bx - ax;
    float dy  = by - ay;
    float len = std::sqrt(dx * dx + dy * dy);
    int   n   = std::clamp(static_cast<int>(std::ceil(len / kDot)), 1, std::max(1, maxDots));
    float h   = kDot * 0.5f;

    for (int i = 0; i <= n; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(n);
        float x = ax + dx * t;
        float y = ay + dy * t;
        if (x < -1.0f || x > W + 1.0f || y < -1.0f || y > H + 1.0f) {
            continue;
        }
        fillRect(ctx, x - h, x + h, y - h, y + h, color);
    }
}

struct Box {
    float sx[8];
    float sy[8];
    float minX, minY, maxX, maxY;
};

void drawEsp(MinecraftUIRenderContext& ctx, ScreenView& sv) {
    auto ciRef = ll::service::getClientInstance();
    if (!ciRef) {
        return;
    }
    ClientInstance& ci = *ciRef;

    LocalPlayer* local = ci.getLocalPlayer();
    if (!local) {
        return;
    }

    // Espaco de desenho real desta tela da UI.
    float W = sv.mSize->x;
    float H = sv.mSize->y;
    auto  gui = ci.getGuiData();
    float uiW = gui->mScreenSizeData->clientUIScreenSize->x;
    float uiH = gui->mScreenSizeData->clientUIScreenSize->y;
    if (W <= 1.0f || H <= 1.0f) {
        W = uiW;
        H = uiH;
    }
    if (W <= 1.0f || H <= 1.0f) {
        return;
    }

    CamData    cam   = readCamera(ci, W, H);
    Level&     level = local->getLevel();
    mce::Color cyan(0.55f, 1.0f, 1.0f, 1.0f);

    bool  log    = gLogNextFrame;
    auto& logger = MyMod::getInstance().getSelf().getLogger();

    Actor* localActor = static_cast<Actor*>(local);

    // 1) projeta a caixa de todos os jogadores
    std::vector<Box> boxes;
    int              found = 0;

    for (Actor* a : level.getRuntimeActorList()) {
        if (!a || a == localActor || !a->isPlayer() || !a->isAlive()) {
            continue;
        }
        ++found;

        auto const& bb = a->getAABB();
        Box         box;
        box.minX = box.minY = 1e9f;
        box.maxX = box.maxY = -1e9f;

        bool ok = true;
        for (int i = 0; i < 8; ++i) {
            Vec3f corner{
                (i & 1) ? bb.max.x : bb.min.x,
                (i & 2) ? bb.max.y : bb.min.y,
                (i & 4) ? bb.max.z : bb.min.z
            };
            if (!project(cam, corner, box.sx[i], box.sy[i])) {
                ok = false; // algum canto atras da camera: pula este jogador
                break;
            }
            box.minX = std::min(box.minX, box.sx[i]);
            box.maxX = std::max(box.maxX, box.sx[i]);
            box.minY = std::min(box.minY, box.sy[i]);
            box.maxY = std::max(box.maxY, box.sy[i]);
        }

        if (log) {
            if (ok) {
                logger.info("jogador {}: caixa ({:.1f},{:.1f}) - ({:.1f},{:.1f}) em {:.0f}x{:.0f}", found, box.minX, box.minY, box.maxX, box.maxY, W, H);
            } else {
                logger.info("jogador {}: atras da camera", found);
            }
        }
        if (!ok || box.maxX < 0.0f || box.minX > W || box.maxY < 0.0f || box.minY > H) {
            continue;
        }
        boxes.push_back(box);
    }

    // 2) desenha
    int perEdge = 40;
    if (!boxes.empty()) {
        perEdge = std::clamp(kRectBudget / (12 * static_cast<int>(boxes.size())), 3, 60);
    }

    for (Box const& b : boxes) {
        if (gMode3D) {
            for (int i = 0; i < 8; ++i) {
                for (int bit : {1, 2, 4}) {
                    if (i & bit) {
                        continue;
                    }
                    int j = i | bit;
                    drawEdge(ctx, b.sx[i], b.sy[i], b.sx[j], b.sy[j], perEdge, W, H, cyan);
                }
            }
        } else {
            drawEdge(ctx, b.minX, b.minY, b.maxX, b.minY, 60, W, H, cyan);
            drawEdge(ctx, b.minX, b.maxY, b.maxX, b.maxY, 60, W, H, cyan);
            drawEdge(ctx, b.minX, b.minY, b.minX, b.maxY, 60, W, H, cyan);
            drawEdge(ctx, b.maxX, b.minY, b.maxX, b.maxY, 60, W, H, cyan);
        }
    }

    if (log) {
        auto  proj    = ci.getCamera().getProjectionMatrix();
        float p11     = std::abs(proj[1][1]);
        float fovMat  = p11 > 1e-4f ? 2.0f * std::atan(1.0f / p11) * 180.0f / 3.14159265f : 0.0f;
        float fovGame = gameFovDegrees(ci);
        logger.info(
            "diagnostico: jogadores={} modo={} fonteFOV={} fovJogo={:.1f} fovMatriz={:.1f} P00={:.3f} P11={:.3f} escala={:.2f}",
            found, gMode3D ? "3D" : "2D", gUseGameFov ? "jogo" : "matriz", fovGame, fovMat, proj[0][0], proj[1][1], gScale
        );
        logger.info("espaco da UI: ScreenView {:.0f}x{:.0f} / clientUI {:.0f}x{:.0f}", W, H, uiW, uiH);
        gLogNextFrame = false;
    }
}

} // namespace

MyMod& MyMod::getInstance() {
    static MyMod instance;
    return instance;
}

bool MyMod::load() {
    getSelf().getLogger().info("Meu ESP: load");
    return true;
}

bool MyMod::enable() {
    getSelf().getLogger().info("Meu ESP: enable - R liga/desliga; N 2D/3D; B fonte do FOV; [ ] escala");
    gActive = true;

    auto& bus = ll::event::EventBus::getInstance();

    bus.emplaceListener<ll::event::KeyInputEvent>([this](ll::event::KeyInputEvent& event) {
        int code = event.keyCode();
        if (!gActive || code < 0 || code > 255) {
            return;
        }

        static bool held[256] = {};
        if (!event.isDown()) {
            held[code] = false;
            return;
        }
        if (held[code]) {
            return; // tecla segurada: ignora repeticao
        }
        held[code] = true;

        auto& logger = getSelf().getLogger();
        switch (code) {
        case 'R':
            gEspOn        = !gEspOn;
            gLogNextFrame = gEspOn;
            logger.info("ESP {}", gEspOn ? "LIGADO" : "DESLIGADO");
            break;
        case 'N':
            gMode3D       = !gMode3D;
            gLogNextFrame = true;
            logger.info("Caixa {}", gMode3D ? "3D (arame)" : "2D simples");
            break;
        case 'B':
            gUseGameFov   = !gUseGameFov;
            gLogNextFrame = true;
            logger.info("FOV vem de: {}", gUseGameFov ? "jogo" : "matriz da camera");
            break;
        case kKeyScaleDown:
            gScale        = std::max(0.2f, gScale * 0.95f);
            gLogNextFrame = true;
            logger.info("Escala geral = {:.3f}", gScale);
            break;
        case kKeyScaleUp:
            gScale        = std::min(5.0f, gScale * 1.05f);
            gLogNextFrame = true;
            logger.info("Escala geral = {:.3f}", gScale);
            break;
        default:
            break;
        }
    });

    bus.emplaceListener<ll::event::AfterUIRenderEvent>([this](ll::event::AfterUIRenderEvent& event) {
        if (!gActive || !gEspOn) {
            return;
        }

        // O evento dispara uma vez por tela. Desenhamos so na tela do HUD.
        // O nome de cada tela so e lido uma vez (guardado por ponteiro) para nao pesar.
        static std::unordered_map<void const*, bool> hudCache;
        ScreenView& sv = event.screenView();

        bool isHud;
        auto it = hudCache.find(&sv);
        if (it != hudCache.end()) {
            isHud = it->second;
        } else {
            std::string name = sv.getScreenName();
            isHud            = name.find("hud") != std::string::npos;
            if (hudCache.size() > 64) {
                hudCache.clear();
            }
            hudCache[&sv] = isHud;

            static std::set<std::string> seen;
            if (seen.size() < 12 && seen.insert(name).second) {
                getSelf().getLogger().info("tela de UI vista: {}", name);
            }
        }

        bool draw = false;
        if (isHud) {
            gSeenHud = true;
        }
        if (gSeenHud) {
            draw = isHud;
        } else if (++gPassesNoHud > 300) {
            // Nenhuma tela com "hud" no nome: desenha uma vez por quadro em qualquer tela.
            auto now = std::chrono::steady_clock::now();
            if (now - gLastDraw >= std::chrono::milliseconds(4)) {
                gLastDraw = now;
                draw      = true;
            }
        }

        if (draw) {
            drawEsp(event.uiRenderContext(), sv);
        }
    });

    return true;
}

bool MyMod::disable() {
    getSelf().getLogger().info("Meu ESP: disable");
    gActive = false;
    gEspOn  = false;
    return true;
}

} // namespace my_mod

LL_REGISTER_MOD(my_mod::MyMod, my_mod::MyMod::getInstance());
