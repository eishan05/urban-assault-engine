#include <stdlib.h>
#include <string.h>
#include <sstream>

#include "../includes.h"
#include "../system/inpt.h"
#include "../system/gfx.h"
#include "../ypabact.h"
#include "../yparobo.h"

#include "rl_bridge.h"

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

namespace RL
{

TBridge Bridge;

#ifdef _WIN32

// The bridge is POSIX-only; on Windows it compiles to a no-op.
void TBridge::PreFrame(TInputState *, int) {}
void TBridge::ApplyMenuAction(UserData *) {}
int TBridge::PostFrame(int ret, int) { return ret; }
void TBridge::InitListen() {}
void TBridge::TryAccept() {}
void TBridge::Disconnect() {}
bool TBridge::ReadLine(std::string *) { return false; }
void TBridge::SendLine(const std::string &) {}
bool TBridge::HandleCommand(const std::string &, TInputState *, int) { return false; }
std::string TBridge::BuildState(int) { return std::string(); }

#else

void TBridge::InitListen()
{
    _listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if ( _listenFd < 0 )
    {
        ypa_log_out("RL bridge: socket() failed (%s)\n", strerror(errno));
        _sockPath.clear();
        return;
    }

    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if ( _sockPath.size() >= sizeof(addr.sun_path) )
    {
        ypa_log_out("RL bridge: socket path too long: %s\n", _sockPath.c_str());
        close(_listenFd);
        _listenFd = -1;
        _sockPath.clear();
        return;
    }

    strcpy(addr.sun_path, _sockPath.c_str());
    unlink(_sockPath.c_str());

    if ( bind(_listenFd, (sockaddr *)&addr, sizeof(addr)) < 0 ||
         listen(_listenFd, 1) < 0 )
    {
        ypa_log_out("RL bridge: bind/listen on %s failed (%s)\n", _sockPath.c_str(), strerror(errno));
        close(_listenFd);
        _listenFd = -1;
        _sockPath.clear();
        return;
    }

    fcntl(_listenFd, F_SETFL, fcntl(_listenFd, F_GETFL, 0) | O_NONBLOCK);

    ypa_log_out("RL bridge: listening on %s\n", _sockPath.c_str());
}

void TBridge::TryAccept()
{
    int fd = accept(_listenFd, NULL, NULL);
    if ( fd < 0 )
        return;

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);

    _clientFd = fd;
    _rdBuf.clear();
    _quit = false;
    _pendingLevel = -1;

    // The client paces the loop from here on - drop the FPS cap so
    // frames run back to back. Restored on disconnect.
    _savedFpsMaxTicks = GFX::Engine.FpsMaxTicks;
    GFX::Engine.FpsMaxTicks = 0;

    ypa_log_out("RL bridge: client connected\n");
}

void TBridge::Disconnect()
{
    if ( _clientFd >= 0 )
    {
        close(_clientFd);
        _clientFd = -1;
    }

    _pendingLevel = -1;

    // A crashed client must not leave the game unpaced. Rendering is
    // deliberately NOT re-enabled here: under SDL's dummy video driver
    // there is no GL context, and resuming RenderGame would segfault.
    GFX::Engine.FpsMaxTicks = _savedFpsMaxTicks;

    ypa_log_out("RL bridge: client disconnected\n");
}

bool TBridge::ReadLine(std::string *out)
{
    for (;;)
    {
        size_t nl = _rdBuf.find('\n');
        if ( nl != std::string::npos )
        {
            *out = _rdBuf.substr(0, nl);
            _rdBuf.erase(0, nl + 1);

            if ( !out->empty() && out->back() == '\r' )
                out->pop_back();

            return true;
        }

        char tmp[4096];
        ssize_t n = recv(_clientFd, tmp, sizeof(tmp), 0);
        if ( n <= 0 )
            return false;

        _rdBuf.append(tmp, n);
    }
}

void TBridge::SendLine(const std::string &line)
{
    std::string msg = line + "\n";

    size_t done = 0;
    while ( done < msg.size() )
    {
        ssize_t n = send(_clientFd, msg.data() + done, msg.size() - done, MSG_NOSIGNAL);
        if ( n <= 0 )
        {
            Disconnect();
            return;
        }
        done += n;
    }
}

static void AppendUnit(std::string *s, NC_STACK_ypabact *u)
{
    *s += fmt::sprintf("{\"gid\":%u,\"ty\":%d,\"vid\":%d,\"own\":%d,\"st\":%d,"
                       "\"e\":%d,\"em\":%d,\"p\":[%.2f,%.2f,%.2f]}",
                       u->_gid, u->_bact_type, (int)u->_vehicleID, (int)u->_owner,
                       (int)u->_status, u->_energy, u->_energy_max,
                       u->_position.x, u->_position.y, u->_position.z);
}

std::string TBridge::BuildState(int screenMode)
{
    std::string s = fmt::sprintf("{\"ok\":true,\"mode\":%d,\"dt\":%u,\"ts\":%u",
                                 screenMode, _fixedDt, world_update_arg.TimeStamp);

    if ( ypaworld )
    {
        const TLevelInfo &li = ypaworld->GetLevelInfo();
        s += fmt::sprintf(",\"level\":{\"id\":%d,\"state\":%d},\"player_owner\":%d",
                          li.LevelID, li.State, (int)ypaworld->GetPlayerOwner());

        if ( screenMode == 2 ) // GAME_SCREEN_MODE_GAME
        {
            NC_STACK_ypabact *u = ypaworld->_userUnit;
            if ( u )
            {
                vec3d v = u->_fly_dir * u->_fly_dir_length;
                s += fmt::sprintf(",\"player\":{\"gid\":%u,\"ty\":%d,\"vid\":%d,\"own\":%d,"
                                  "\"st\":%d,\"e\":%d,\"em\":%d,"
                                  "\"p\":[%.3f,%.3f,%.3f],\"v\":[%.3f,%.3f,%.3f],"
                                  "\"rot\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f],"
                                  "\"sec\":[%d,%d]}",
                                  u->_gid, u->_bact_type, (int)u->_vehicleID, (int)u->_owner,
                                  (int)u->_status, u->_energy, u->_energy_max,
                                  u->_position.x, u->_position.y, u->_position.z,
                                  v.x, v.y, v.z,
                                  u->_rotation.m00, u->_rotation.m01, u->_rotation.m02,
                                  u->_rotation.m10, u->_rotation.m11, u->_rotation.m12,
                                  u->_rotation.m20, u->_rotation.m21, u->_rotation.m22,
                                  u->_cellId.x, u->_cellId.y);
            }

            NC_STACK_ypabact *r = ypaworld->_userRobo;
            if ( r )
                s += fmt::sprintf(",\"robo\":{\"gid\":%u,\"e\":%d,\"em\":%d,"
                                  "\"p\":[%.2f,%.2f,%.2f],\"sec\":[%d,%d]}",
                                  r->_gid, r->_energy, r->_energy_max,
                                  r->_position.x, r->_position.y, r->_position.z,
                                  r->_cellId.x, r->_cellId.y);

            if ( _obsUnits )
            {
                s += ",\"units\":[";
                bool first = true;
                for ( NC_STACK_ypabact *unit : ypaworld->_unitsList )
                {
                    if ( !first )
                        s += ",";
                    first = false;
                    AppendUnit(&s, unit);
                }
                s += "]";
            }

            if ( _obsSectors && ypaworld->_mapSize.x > 0 )
            {
                s += fmt::sprintf(",\"sectors\":{\"w\":%d,\"h\":%d,\"owner\":[",
                                  ypaworld->_mapSize.x, ypaworld->_mapSize.y);

                for ( int y = 0; y < ypaworld->_mapSize.y; y++ )
                    for ( int x = 0; x < ypaworld->_mapSize.x; x++ )
                        s += fmt::sprintf(y + x ? ",%d" : "%d",
                                          (int)ypaworld->SectorAt(x, y).owner);

                s += "],\"energy\":[";

                for ( int y = 0; y < ypaworld->_mapSize.y; y++ )
                    for ( int x = 0; x < ypaworld->_mapSize.x; x++ )
                        s += fmt::sprintf(y + x ? ",%d" : "%d",
                                          ypaworld->SectorAt(x, y).energy_power);

                s += "]}";
            }
        }
    }

    s += "}";
    return s;
}

bool TBridge::HandleCommand(const std::string &line, TInputState *inp, int screenMode)
{
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;

    if ( cmd == "INFO" )
    {
        SendLine(BuildState(screenMode));
        return false;
    }

    if ( cmd == "CFG" )
    {
        std::string kv;
        while ( ss >> kv )
        {
            size_t eq = kv.find('=');
            if ( eq == std::string::npos )
                continue;

            std::string key = kv.substr(0, eq);
            int val = atoi(kv.c_str() + eq + 1);

            if ( key == "dt" && val > 0 && val <= 2000 )
                _fixedDt = val;
            else if ( key == "headless" && ypaworld )
                ypaworld->setYW_dontRender(val != 0);
            else if ( key == "units" )
                _obsUnits = val != 0;
            else if ( key == "sectors" )
                _obsSectors = val != 0;
            else if ( key == "seed" )
                srand(val);
        }

        SendLine("{\"ok\":true}");
        return false;
    }

    if ( cmd == "STEP" )
    {
        float s0 = 0.0, s1 = 0.0, s2 = 0.0;
        unsigned int btn = 0;
        int key = 0;
        ss >> s0 >> s1 >> s2 >> btn >> key;

        *inp = TInputState();
        inp->Sliders[0] = s0;
        inp->Sliders[1] = s1;
        inp->Sliders[2] = s2;

        for ( unsigned int i = 0; i < 32; i++ )
        {
            if ( btn & (1u << i) )
                inp->Buttons.Set(i);
        }

        if ( key > 0 )
        {
            inp->KbdLastHit = key;
            inp->KbdLastDown = key;
        }

        return true;
    }

    if ( cmd == "START" )
    {
        int lvl = -1;
        ss >> lvl;

        if ( lvl < 0 )
        {
            SendLine("{\"ok\":false,\"error\":\"START needs a level id\"}");
            return false;
        }

        // Pending levels are only applied from the menu frame; accepting
        // START elsewhere would leave it queued for a later menu
        // transition while the current level keeps running.
        if ( screenMode != 1 ) // GAME_SCREEN_MODE_MENU
        {
            SendLine("{\"ok\":false,\"error\":\"START only valid in the menu (ABORT first)\"}");
            return false;
        }

        _pendingLevel = lvl;
        *inp = TInputState();
        return true;
    }

    if ( cmd == "RESET" || cmd == "ABORT" || cmd == "SAVE" || cmd == "LOAD" )
    {
        if ( screenMode != 2 || !ypaworld )
        {
            SendLine(fmt::sprintf("{\"ok\":false,\"error\":\"%s needs a running level\"}", cmd));
            return false;
        }

        TLevelInfo &li = ypaworld->GetLevelInfo();

        if ( cmd == "RESET" )
            li.State = TLevelInfo::STATE_RESTART;
        else if ( cmd == "ABORT" )
            li.State = TLevelInfo::STATE_ABORTED;
        else if ( cmd == "SAVE" )
            li.State = TLevelInfo::STATE_SAVE;
        else
            li.State = TLevelInfo::STATE_LOAD;

        *inp = TInputState();
        return true;
    }

    if ( cmd == "PROTOS" )
    {
        if ( !ypaworld || ypaworld->_vhclProtos.empty() )
        {
            SendLine("{\"ok\":false,\"error\":\"no vehicle prototypes loaded (start a level)\"}");
            return false;
        }

        std::string s = "{\"ok\":true,\"protos\":[";
        bool first = true;
        for ( size_t i = 0; i < ypaworld->_vhclProtos.size(); i++ )
        {
            const World::TVhclProto &p = ypaworld->_vhclProtos[i];
            if ( p.model_id <= 0 )
                continue;

            if ( !first )
                s += ",";
            first = false;
            s += fmt::sprintf("{\"vid\":%u,\"model\":%d,\"name\":\"%s\",\"energy\":%d,"
                              "\"force\":%.1f,\"mass\":%.1f,\"maxrot\":%.4f}",
                              i, p.model_id, p.name, p.energy, p.force, p.mass, p.maxrot);
        }
        s += "]}";
        SendLine(s);
        return false;
    }

    if ( cmd == "SPAWN" )
    {
        // Create a vehicle next to the user's host station and put the
        // player into it (the level-start user unit is the host station
        // robo, which ignores manual driving input).
        int vid = -1;
        ss >> vid;

        float dx, dz;
        const float dy = 200.0;
        if ( !(ss >> dx) )
            dx = 600.0;
        if ( !(ss >> dz) )
            dz = 600.0;

        NC_STACK_ypabact *robo = ypaworld ? ypaworld->_userRobo : NULL;

        if ( screenMode != 2 || !robo )
        {
            SendLine("{\"ok\":false,\"error\":\"SPAWN needs a running level\"}");
            return false;
        }

        if ( vid < 0 || (size_t)vid >= ypaworld->_vhclProtos.size() ||
             ypaworld->_vhclProtos[vid].model_id <= 0 )
        {
            SendLine(fmt::sprintf("{\"ok\":false,\"error\":\"bad vehicle id %d\"}", vid));
            return false;
        }

        ypaworld_arg146 arg146;
        arg146.vehicle_id = vid;
        arg146.pos = robo->_position + vec3d(dx, dy, dz);

        NC_STACK_ypabact *unit = ypaworld->ypaworld_func146(&arg146);
        if ( !unit )
        {
            SendLine(fmt::sprintf("{\"ok\":false,\"error\":\"vehicle %d creation failed\"}", vid));
            return false;
        }

        unit->_owner = robo->_owner;
        unit->_host_station = static_cast<NC_STACK_yparobo *>(robo);
        unit->setBACT_bactCollisions(robo->getBACT_bactCollisions());

        // Parent it to the host station (squad-leader position in the
        // unit hierarchy). AI code like GetEnemyCandidateInSector
        // dereferences _parent unconditionally for non-robo units; a
        // parentless vehicle segfaults the sim as soon as an enemy
        // enters sensor range.
        robo->AddSubject(unit);

        NC_STACK_ypabact *prev = ypaworld->_userUnit;
        if ( prev )
        {
            prev->setBACT_inputting(false);
            prev->setBACT_viewer(false);
        }

        unit->setBACT_viewer(true);
        unit->setBACT_inputting(true);

        *inp = TInputState();
        return true;
    }

    if ( cmd == "QUIT" )
    {
        _quit = true;
        *inp = TInputState();
        return true;
    }

    SendLine(fmt::sprintf("{\"ok\":false,\"error\":\"unknown command %s\"}", cmd));
    return false;
}

void TBridge::PreFrame(TInputState *inp, int screenMode)
{
    if ( !_envChecked )
    {
        _envChecked = true;

        const char *path = getenv("UA_RL_SOCKET");
        if ( path && path[0] )
        {
            _sockPath = path;
            InitListen();
        }
    }

    if ( _listenFd < 0 )
        return;

    if ( _clientFd < 0 )
    {
        TryAccept();
        if ( _clientFd < 0 )
            return;
    }

    for (;;)
    {
        std::string line;
        if ( !ReadLine(&line) )
        {
            Disconnect();
            return;
        }

        if ( HandleCommand(line, inp, screenMode) )
            break;
    }

    // Deterministic fixed timestep - replaces the wall-clock Period
    // measured by QueryInput (already incremented by the caller).
    inp->Period = _fixedDt;
}

void TBridge::ApplyMenuAction(UserData *usr)
{
    if ( _clientFd < 0 )
        return;

    if ( _pendingLevel >= 0 )
    {
        usr->envAction.action = EnvAction::ACTION_PLAY;
        usr->envAction.params[0] = _pendingLevel;
        usr->envAction.params[1] = _pendingLevel;

        _pendingLevel = -1;
    }

    if ( _quit )
        usr->envAction.action = EnvAction::ACTION_QUIT;
}

int TBridge::PostFrame(int ret, int screenMode)
{
    if ( _clientFd < 0 )
        return ret;

    SendLine(BuildState(screenMode));

    if ( _quit )
        return 0;

    return ret;
}

#endif // _WIN32

}
