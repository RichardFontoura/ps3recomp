#ifndef PS3RECOMP_PS3_AB_H
#define PS3RECOMP_PS3_AB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3_ab_switch {
    const char* name;
    void (*set)(int on);
} ps3_ab_switch;

unsigned ps3_ab_parse(const char* spec, const ps3_ab_switch* table, unsigned count);
int ps3_ab_start(const ps3_ab_switch* table, unsigned count, unsigned (*frames)(void));

#ifdef __cplusplus
}
#endif

#endif
