#ifndef ANIM_H
#define ANIM_H

#include "raylib.h"
#include <stdbool.h>

/* ================================================================
 * Animation intensity — persisted in Player.anim_intensity.
 * ================================================================ */
typedef enum {
    ANIM_INTENSITY_FULL    = 0,
    ANIM_INTENSITY_REDUCED = 1,
    ANIM_INTENSITY_OFF     = 2
} AnimIntensity;

void          anim_set_intensity(AnimIntensity i);
AnimIntensity anim_get_intensity(void);
float         anim_scale(void);
bool          anim_enabled(void);
bool          anim_full(void);

/* ================================================================
 * Easing
 * ================================================================ */
typedef float (*EaseFn)(float);

float ease_linear(float t);
float ease_out_quad(float t);
float ease_out_cubic(float t);
float ease_out_quint(float t);
float ease_in_cubic(float t);
float ease_in_out_cubic(float t);
float ease_out_back(float t);
float ease_in_back(float t);
float ease_out_elastic(float t);

/* ================================================================
 * Scalar helpers
 * ================================================================ */
float anim_clamp01(float v);
float anim_lerp(float a, float b, float t);
Color anim_color_alpha(Color c, float a);
Color anim_color_lerp(Color a, Color b, float t);

/* ================================================================
 * Timer
 * ================================================================ */
typedef struct Timer { float duration, elapsed; bool done; } Timer;
void  timer_start(Timer *t, float duration);
bool  timer_update(Timer *t, float dt);   /* true the frame it finishes */
float timer_progress(const Timer *t);

/* ================================================================
 * Tween
 * ================================================================ */
typedef struct Tween {
    float from, to;
    float duration, elapsed, delay;
    bool  active;
    EaseFn ease;
} Tween;

void  tween_to(Tween *tw, float to, float duration, float delay);
void  tween_to_eased(Tween *tw, float to, float duration, float delay, EaseFn ease);
void  tween_snap(Tween *tw, float v);
float tween_update(Tween *tw, float dt);

/* ================================================================
 * Spring — critically damped or underdamped harmonic oscillator.
 * Typical values: stiffness 180–400, damping 10–18.
 * ================================================================ */
typedef struct Spring {
    float value;
    float velocity;
    float target;
    float stiffness;
    float damping;
} Spring;

void  spring_init(Spring *s, float value, float stiffness, float damping);
void  spring_set(Spring *s, float target);
void  spring_snap(Spring *s, float value);
float spring_update(Spring *s, float dt);
bool  spring_settled(const Spring *s);

/* ================================================================
 * Particles
 * ================================================================ */
typedef enum {
    P_SHAPE_CIRCLE = 0,
    P_SHAPE_SQUARE,
    P_SHAPE_STAR,
    P_SHAPE_SPARK,
    P_SHAPE_GLOW
} ParticleShape;

#define ANIM_MAX_PARTICLES 512

typedef struct Particle {
    Vector2  pos, vel;
    float    life, max_life;
    float    size, size_end;
    float    gravity, drag;
    float    rotation, rot_speed;
    Color    color;
    ParticleShape shape;
    bool     alive;
} Particle;

typedef struct ParticleSystem {
    Particle items[ANIM_MAX_PARTICLES];
    int      cursor;
} ParticleSystem;

void particles_reset(ParticleSystem *ps);
void particles_burst(ParticleSystem *ps, Vector2 pos, int count, Color c,
                     float min_speed, float max_speed, float life);
void particles_burst_shaped(ParticleSystem *ps, Vector2 pos, int count,
                            Color c, float min_speed, float max_speed,
                            float life, ParticleShape shape,
                            float min_size, float max_size,
                            float size_end, float gravity);
void particles_burst_ring(ParticleSystem *ps, Vector2 pos, int count,
                          Color c, float speed, float life,
                          ParticleShape shape, float size);
void particles_update(ParticleSystem *ps, float dt);
void particles_draw(const ParticleSystem *ps);
int  particles_alive(const ParticleSystem *ps);

/* ================================================================
 * Screen shake
 * ================================================================ */
typedef struct Shake { float amount, max, decay; } Shake;
void    shake_reset(Shake *s);
void    shake_add(Shake *s, float amount);
void    shake_update(Shake *s, float dt);
Vector2 shake_offset(const Shake *s);

/* ================================================================
 * Screen transition
 * ================================================================ */
typedef enum {
    TRANS_FADE = 0,
    TRANS_SLIDE_RIGHT,
    TRANS_SLIDE_LEFT,
    TRANS_SCALE
} TransitionStyle;

#define TRANS_DEFAULT_DUR 0.28f

typedef struct TransEffect {
    float   alpha;
    Vector2 offset;
    float   zoom;
} TransEffect;

TransEffect trans_evaluate(float screen_t, float duration,
                           TransitionStyle style, int W, int H);

/* ================================================================
 * Ambient background — draws 6 slowly-drifting soft circles.
 * Call once at the start of any screen that wants ambient motion.
 * Cost: 6 DrawCircle calls. No-op when animation is OFF.
 * ================================================================ */
void anim_draw_ambient_bg(int W, int H, float time);

#endif
