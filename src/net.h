// Due console, una partita: lockstep sugli input. Vedi net.c
#pragma once
typedef struct { signed char x, y; unsigned short buttons; } NetMove;  // direzione -127..127, tasti del pad (bit = GamepadButton)

extern int net_on, net_role;  // net_role: 0 chi ospita, 1 chi si unisce
int net_open(const char *peer, unsigned *seed);  // sviluppo: peer NULL = ospita. Aspetta l'altra console, sceglie il seme comune

// Lobby: chi ospita si annuncia sulla rete locale, chi si unisce lo trova e manda il proprio equipaggiamento (testo JSON).
// Quando l'ospite parte manda seme e salvataggio; da li' in poi net_on e' vero per entrambe.
int net_lobby_start(int host);
int net_lobby_poll(const char *hello, char *out, int out_size);  // 1 = l'altra console c'e'; per chi ospita, out e' il suo equipaggiamento
void net_lobby_resend(const char *hello);
int net_lobby_send_go(unsigned seed, const char *text);
int net_lobby_wait_go(unsigned *seed, char **text);  // 1 = partita; text da liberare con free
void net_begin(void);
void net_close(void);
int net_lost(void);  // 1 = l'altra console non risponde da 10 s
void net_submit(NetMove local);                  // l'input di questa console per il frame futuro
int net_tick(unsigned frame);                    // riceve e invia; 1 quando si puo' simulare il frame
NetMove net_move(int player, unsigned frame);
void net_frame_hash(unsigned frame, unsigned hash);  // confronta lo stato con l'altra console
int net_wants_input(unsigned frame);
void net_report(unsigned frame);

// Dentro la simulazione i tasti sono quelli arrivati dalla rete, del giocatore che guida la scena: cosi' le due console fanno gli stessi passi.
// Tocco, mouse e tastiera non entrano in simulazione.
extern int net_in_sim, net_input_player;
extern unsigned net_input_frame;
int net_pad_down(int button);
int net_pad_pressed(int button);
int net_pad_released(int button);
float net_pad_axis(int axis);
int net_player_pressed(int player, int button);
#ifdef NET_WRAP_INPUT
#define IsKeyPressed(k) (net_in_sim ? 0 : (IsKeyPressed)(k))
#define IsKeyDown(k) (net_in_sim ? 0 : (IsKeyDown)(k))
#define GetKeyPressed() (net_in_sim ? 0 : (GetKeyPressed)())
#define GetCharPressed() (net_in_sim ? 0 : (GetCharPressed)())
#define GetTouchPointCount() (net_in_sim ? 0 : (GetTouchPointCount)())
#define IsMouseButtonDown(b) (net_in_sim ? 0 : (IsMouseButtonDown)(b))
#define IsMouseButtonPressed(b) (net_in_sim ? 0 : (IsMouseButtonPressed)(b))
#define GetMouseWheelMove() (net_in_sim ? 0 : (GetMouseWheelMove)())
#define IsGamepadAvailable(g) (net_in_sim ? 1 : (IsGamepadAvailable)(g))
#define IsGamepadButtonDown(g, b) (net_in_sim ? net_pad_down(b) : (IsGamepadButtonDown)(g, b))
#define IsGamepadButtonPressed(g, b) (net_in_sim ? net_pad_pressed(b) : (IsGamepadButtonPressed)(g, b))
#define IsGamepadButtonReleased(g, b) (net_in_sim ? net_pad_released(b) : (IsGamepadButtonReleased)(g, b))
#define GetGamepadAxisMovement(g, a) (net_in_sim ? net_pad_axis(a) : (GetGamepadAxisMovement)(g, a))
#endif
