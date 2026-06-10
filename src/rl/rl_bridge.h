#ifndef RL_BRIDGE_H_INCLUDED
#define RL_BRIDGE_H_INCLUDED

#include <string>
#include <stdint.h>

struct TInputState;
class UserData;

namespace RL
{

/*
 * Lockstep RL control bridge over a Unix domain socket.
 *
 * Inactive unless the UA_RL_SOCKET environment variable holds a socket
 * path. When set, the engine listens there and runs normally until a
 * client connects; from then on every engine frame consumes exactly one
 * frame-running command and answers with one JSON state line, so the
 * client fully paces the simulation (faster or slower than realtime).
 *
 * Client -> engine, one command per '\n'-terminated line:
 *   INFO                              state reply, no frame
 *   CFG k=v [k=v ...]                 no frame; keys:
 *                                       dt=<ms>      fixed sim timestep (default 20)
 *                                       headless=0|1 setYW_dontRender
 *                                       units=0|1    include units array in state
 *                                       sectors=0|1  include sector grid in state
 *                                       seed=<n>     srand(n)
 *   START <levelID>                   runs one frame; level launch is applied
 *                                     at the next menu frame
 *   STEP <s0> <s1> <s2> <btn> <key>   runs one frame with Sliders[0..2] =
 *                                     turn/pitch/throttle, <btn> = Buttons
 *                                     bitmask, <key> = engine keycode (0=none)
 *   RESET | ABORT | SAVE | LOAD       runs one frame after setting
 *                                     TLevelInfo::State accordingly (game mode)
 *   PROTOS                            list vehicle prototypes, no frame
 *   SPAWN <vid> [dx] [dz]             runs one frame; creates vehicle <vid>
 *                                     at host station + (dx, 200, dz)
 *                                     (default 600,600) and transfers user
 *                                     control/view into it
 *   QUIT                              clean engine shutdown
 *
 * Engine -> client: one JSON object per line (see BuildState).
 */
class TBridge
{
public:
    // Called in ProcessNextFrame after the Period increment; blocks for
    // one frame-running command while a client is connected and applies
    // it to the input state.
    void PreFrame(TInputState *inp, int screenMode);

    // Called in ProcessMenuFrame after ProcessGameShell() (which clears
    // envAction every frame) to apply a pending START/QUIT.
    void ApplyMenuAction(UserData *usr);

    // Called at the end of ProcessNextFrame; sends the state reply.
    // Returns 0 (engine exit) when a QUIT was requested, else ret.
    int PostFrame(int ret, int screenMode);

private:
    void InitListen();
    void TryAccept();
    void Disconnect();
    bool ReadLine(std::string *out);
    void SendLine(const std::string &line);
    // true if the command consumes a frame
    bool HandleCommand(const std::string &line, TInputState *inp, int screenMode);
    std::string BuildState(int screenMode);

    bool _envChecked = false;
    std::string _sockPath;
    int _listenFd = -1;
    int _clientFd = -1;
    std::string _rdBuf;

    uint32_t _fixedDt = 20;
    uint32_t _savedFpsMaxTicks = 0;
    bool _quit = false;
    int _pendingLevel = -1;

    bool _obsUnits = true;
    bool _obsSectors = false;
};

extern TBridge Bridge;

}

#endif // RL_BRIDGE_H_INCLUDED
