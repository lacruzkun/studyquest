#include "anim.h"
#include <math.h>
#include <string.h>

/* ---------------- intensity ---------------- */
static AnimIntensity g_intensity = ANIM_INTENSITY_FULL;

void anim_set_intensity(AnimIntensity i) { g_intensity = i; }
AnimIntensity anim_get_intensity(void)   { return g_intensity; }

float anim_scale(void) {
    switch (g_intensity) {
        case ANIM_INTENSITY_FULL:    return 1.0f;
        case ANIM_INTENSITY_REDUCED: return 0.4f;
        case ANIM_INTENSITY_OFF:     return 0.0f;
    }
    return 1.0f;
}
bool anim_enabled(void) { return g_intensity != ANIM_INTENSITY_OFF; }
bool anim_full(void)    { return g_intensity == ANIM_INTENSITY_FULL; }

/* ---------------- helpers ---------------- */
float anim_clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
float anim_lerp(float a, float b, float t) { return a + (b - a) * t; }

Color anim_color_alpha(Color c, float a) {
    a = anim_clamp01(a);
    c.a = (unsigned char)(c.a * a);
    return c;
}
Color anim_color_lerp(Color a, Color b, float t) {
    t = anim_clamp01(t);
    return (Color){
        (unsigned char)(a.r + (b.r - a.r) * t),
        (unsigned char)(a.g + (b.g - a.g) * t),
        (unsigned char)(a.b + (b.b - a.b) * t),
        (unsigned char)(a.a + (b.a - a.a) * t),
    };
}

/* ---------------- easing ---------------- */
float ease_linear(float t) { return anim_clamp01(t); }
float ease_out_quad(float t) { t = anim_clamp01(t); return 1.f - (1.f-t)*(1.f-t); }
float ease_out_cubic(float t) { t = anim_clamp01(t); float u = 1.f-t; return 1.f - u*u*u; }
float ease_out_quint(float t) { t = anim_clamp01(t); float u = 1.f-t; return 1.f - u*u*u*u*u; }
float ease_in_cubic(float t) { t = anim_clamp01(t); return t*t*t; }
float ease_in_out_cubic(float t) {
    t = anim_clamp01(t);
    return t < 0.5f ? 4.f*t*t*t : 1.f - powf(-2.f*t + 2.f, 3.f) / 2.f;
}
float ease_out_back(float t) {
    t = anim_clamp01(t);
    const float c1 = 1.70158f, c3 = c1 + 1.f;
    float u = t - 1.f;
    return 1.f + c3 * u*u*u + c1 * u*u;
}
float ease_in_back(float t) {
    t = anim_clamp01(t);
    const float c1 = 1.70158f, c3 = c1 + 1.f;
    return c3 * t*t*t - c1 * t*t;
}
float ease_out_elastic(float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    const float c4 = (2.f * PI) / 3.f;
    return powf(2.f, -10.f*t) * sinf((t*10.f - 0.75f) * c4) + 1.f;
}

/* ---------------- timer ---------------- */
void timer_start(Timer *t, float duration) {
    t->duration = duration > 0.f ? duration : 0.0001f;
    t->elapsed = 0.f;
    t->done = false;
}
bool timer_update(Timer *t, float dt) {
    if (t->done) return false;
    t->elapsed += dt;
    if (t->elapsed >= t->duration) {
        t->elapsed = t->duration;
        t->done = true;
        return true;
    }
    return false;
}
float timer_progress(const Timer *t) {
    if (t->duration <= 0.f) return 1.f;
    return anim_clamp01(t->elapsed / t->duration);
}

/* ---------------- tween ---------------- */
void tween_to(Tween *tw, float to, float duration, float delay) {
    tween_to_eased(tw, to, duration, delay, ease_out_cubic);
}
void tween_to_eased(Tween *tw, float to, float duration, float delay, EaseFn ease) {
    tw->from = tw->to;
    tw->to = to;
    tw->duration = duration > 0.f ? duration : 0.0001f;
    tw->elapsed = 0.f;
    tw->delay = delay;
    tw->ease = ease;
    tw->active = true;
}
void tween_snap(Tween *tw, float v) {
    tw->from = tw->to = v;
    tw->elapsed = tw->duration;
    tw->active = false;
}
float tween_update(Tween *tw, float dt) {
    if (!tw->active) return tw->to;
    if (tw->delay > 0.f) {
        tw->delay -= dt;
        if (tw->delay > 0.f) return tw->from;
        dt = -tw->delay;
        tw->delay = 0.f;
    }
    tw->elapsed += dt;
    if (tw->elapsed >= tw->duration) {
        tw->active = false;
        return tw->to;
    }
    float p = tw->elapsed / tw->duration;
    EaseFn e = tw->ease ? tw->ease : ease_out_cubic;
    return tw->from + (tw->to - tw->from) * e(p);
}

/* ---------------- spring ---------------- */
void spring_init(Spring *s, float value, float stiffness, float damping) {
    s->value = value;
    s->target = value;
    s->velocity = 0.f;
    s->stiffness = stiffness;
    s->damping = damping;
}
void spring_set(Spring *s, float target) { s->target = target; }
void spring_snap(Spring *s, float value) {
    s->value = s->target = value;
    s->velocity = 0.f;
}
float spring_update(Spring *s, float dt) {
    if (dt > 0.05f) dt = 0.05f;   /* stability clamp */
    float f = -s->stiffness * (s->value - s->target) - s->damping * s->velocity;
    s->velocity += f * dt;
    s->value    += s->velocity * dt;
    if (fabsf(s->value - s->target) < 0.0005f && fabsf(s->velocity) < 0.001f) {
        s->value = s->target;
        s->velocity = 0.f;
    }
    return s->value;
}
bool spring_settled(const Spring *s) {
    return fabsf(s->value - s->target) < 0.001f && fabsf(s->velocity) < 0.001f;
}

/* ---------------- particles ---------------- */
void particles_reset(ParticleSystem *ps) { memset(ps, 0, sizeof(*ps)); }

static Particle *alloc_particle(ParticleSystem *ps) {
    Particle *p = &ps->items[ps->cursor];
    ps->cursor = (ps->cursor + 1) % ANIM_MAX_PARTICLES;
    memset(p, 0, sizeof(*p));
    return p;
}

void particles_burst(ParticleSystem *ps, Vector2 pos, int count, Color c,
                     float min_speed, float max_speed, float life) {
    particles_burst_shaped(ps, pos, count, c, min_speed, max_speed, life,
                           P_SHAPE_CIRCLE, 2.f, 6.f, 0.f, 200.f);
}

void particles_burst_shaped(ParticleSystem *ps, Vector2 pos, int count,
                            Color c, float min_speed, float max_speed,
                            float life, ParticleShape shape,
                            float min_size, float max_size,
                            float size_end, float gravity) {
    if (!anim_enabled()) return;
    count = (int)(count * anim_scale());
    if (count < 1) count = 1;
    for (int i = 0; i < count; i++) {
        Particle *p = alloc_particle(ps);
        float ang = (float)GetRandomValue(0, 3600) / 10.f * DEG2RAD;
        float spd = min_speed + (float)GetRandomValue(0, 1000)/1000.f * (max_speed - min_speed);
        p->pos = pos;
        p->vel = (Vector2){ cosf(ang)*spd, sinf(ang)*spd };
        p->max_life = life * (0.7f + (float)GetRandomValue(0, 600)/1000.f);
        p->life = p->max_life;
        p->size = min_size + (float)GetRandomValue(0, 1000)/1000.f * (max_size - min_size);
        p->size_end = size_end;
        p->gravity = gravity;
        p->drag = 0.6f;
        p->color = c;
        p->shape = shape;
        p->rotation = (float)GetRandomValue(0, 360);
        p->rot_speed = (float)GetRandomValue(-360, 360);
        p->alive = true;
    }
}

void particles_burst_ring(ParticleSystem *ps, Vector2 pos, int count, Color c,
                          float speed, float life, ParticleShape shape, float size) {
    if (!anim_enabled()) return;
    count = (int)(count * anim_scale());
    if (count < 1) return;
    for (int i = 0; i < count; i++) {
        Particle *p = alloc_particle(ps);
        float ang = (float)i / (float)count * 2.f * PI;
        p->pos = pos;
        p->vel = (Vector2){ cosf(ang)*speed, sinf(ang)*speed };
        p->max_life = life;
        p->life = life;
        p->size = size;
        p->size_end = 0.f;
        p->gravity = 0.f;
        p->drag = 2.f;
        p->color = c;
        p->shape = shape;
        p->rotation = ang * RAD2DEG;
        p->rot_speed = 0.f;
        p->alive = true;
    }
}

void particles_update(ParticleSystem *ps, float dt) {
    for (int i = 0; i < ANIM_MAX_PARTICLES; i++) {
        Particle *p = &ps->items[i];
        if (!p->alive) continue;
        p->life -= dt;
        if (p->life <= 0.f) { p->alive = false; continue; }
        float drag = 1.f - p->drag * dt;
        if (drag < 0.f) drag = 0.f;
        p->vel.x *= drag;
        p->vel.y *= drag;
        p->vel.y += p->gravity * dt;
        p->pos.x += p->vel.x * dt;
        p->pos.y += p->vel.y * dt;
        p->rotation += p->rot_speed * dt;
    }
}

static void draw_star(Vector2 c, float r, Color col, float rot_rad) {
    Vector2 pts[10];
    for (int i = 0; i < 10; i++) {
        float a = rot_rad + (float)i * PI / 5.f - PI / 2.f;
        float rad = (i % 2 == 0) ? r : r * 0.45f;
        pts[i] = (Vector2){ c.x + cosf(a) * rad, c.y + sinf(a) * rad };
    }
    for (int i = 0; i < 10; i++)
        DrawTriangle(c, pts[i], pts[(i + 1) % 10], col);
}

void particles_draw(const ParticleSystem *ps) {
    for (int i = 0; i < ANIM_MAX_PARTICLES; i++) {
        const Particle *p = &ps->items[i];
        if (!p->alive) continue;
        float t = p->life / p->max_life;
        float inv = 1.f - t;
        float size = p->size + (p->size_end - p->size) * inv;
        if (size <= 0.1f) continue;
        Color c = p->color;
        c.a = (unsigned char)(c.a * t);
        switch (p->shape) {
            case P_SHAPE_CIRCLE: DrawCircleV(p->pos, size, c); break;
            case P_SHAPE_SQUARE:
                DrawRectanglePro(
                    (Rectangle){ p->pos.x, p->pos.y, size*2.f, size*2.f },
                    (Vector2){ size, size }, p->rotation, c);
                break;
            case P_SHAPE_STAR:
                draw_star(p->pos, size, c, p->rotation * DEG2RAD);
                break;
            case P_SHAPE_SPARK: {
                float dx = cosf(p->rotation * DEG2RAD);
                float dy = sinf(p->rotation * DEG2RAD);
                DrawLineEx((Vector2){ p->pos.x - dx*size, p->pos.y - dy*size },
                           (Vector2){ p->pos.x + dx*size, p->pos.y + dy*size },
                           2.f, c);
                break;
            }
            case P_SHAPE_GLOW: {
                Color gc = c; gc.a = (unsigned char)(c.a * 0.35f);
                DrawCircleV(p->pos, size * 1.8f, gc);
                DrawCircleV(p->pos, size, c);
                break;
            }
        }
    }
}

int particles_alive(const ParticleSystem *ps) {
    int n = 0;
    for (int i = 0; i < ANIM_MAX_PARTICLES; i++) if (ps->items[i].alive) n++;
    return n;
}

/* ---------------- shake ---------------- */
void shake_reset(Shake *s) { s->amount = 0.f; s->max = 0.f; s->decay = 28.f; }
void shake_add(Shake *s, float amount) {
    if (!anim_enabled()) return;
    amount *= anim_scale();
    s->amount += amount;
    if (s->amount > s->max) s->max = s->amount;
}
void shake_update(Shake *s, float dt) {
    if (s->amount <= 0.f) return;
    s->amount -= s->decay * dt * (0.4f + 0.6f * (s->max > 0.f ? s->amount / s->max : 0.f));
    if (s->amount < 0.f) s->amount = 0.f;
    if (s->amount < 0.05f) { s->amount = 0.f; s->max = 0.f; }
}
Vector2 shake_offset(const Shake *s) {
    if (s->amount <= 0.01f) return (Vector2){0, 0};
    return (Vector2){
        (float)GetRandomValue(-100, 100) / 100.f * s->amount,
        (float)GetRandomValue(-100, 100) / 100.f * s->amount
    };
}

/* ---------------- screen transitions ---------------- */
TransEffect trans_evaluate(float screen_t, float duration,
                           TransitionStyle style, int W, int H) {
    (void)H;
    TransEffect e = { .alpha = 0.f, .offset = {0, 0}, .zoom = 1.f };
    if (screen_t >= duration) return e;
    if (!anim_enabled())      return e;

    float p = anim_clamp01(screen_t / duration);
    float eased = ease_out_cubic(p);

    switch (style) {
        case TRANS_FADE:        e.alpha = 1.f - eased; break;
        case TRANS_SLIDE_RIGHT: e.offset.x = (1.f - eased) * W * 0.35f; e.alpha = (1.f - eased) * 0.9f; break;
        case TRANS_SLIDE_LEFT:  e.offset.x = -(1.f - eased) * W * 0.35f; e.alpha = (1.f - eased) * 0.9f; break;
        case TRANS_SCALE:       e.zoom = 0.94f + 0.06f * eased; e.alpha = (1.f - eased) * 0.85f; break;
    }
    return e;
}

/* ---------------- ambient background ---------------- */
void anim_draw_ambient_bg(int W, int H, float time) {
    if (!anim_enabled()) return;

    /* 6 soft drifting circles + 3 slow moving gradient bands. Cheap. */
    static const Color TINTS[3] = {
        { 108, 132, 255, 0 },
        { 240, 120, 160, 0 },
        { 255, 190,  90, 0 },
    };
    float scale = anim_scale();

    for (int i = 0; i < 6; i++) {
        float seed = (float)i * 1.37f;
        float x = (sinf(time * 0.11f + seed) * 0.5f + 0.5f) * (float)W;
        float y = (cosf(time * 0.09f + seed * 1.3f) * 0.5f + 0.5f) * (float)H;
        float r = 220.f + 80.f * sinf(time * 0.13f + seed);
        Color c = TINTS[i % 3];
        c.a = (unsigned char)(10.f * scale);
        DrawCircleV((Vector2){x, y}, r, c);
    }
}
