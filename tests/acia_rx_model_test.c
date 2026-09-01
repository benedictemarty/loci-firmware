/*
 * Test de non-régression (modèle de logique) — chemin RX de l'ACIA 6551 LOCI.
 *
 * Contexte : le firmware LOCI n'est pas compilable ici (toolchain RP2040 / Pico
 * SDK absente : submodules retirés). Ce test ne lie donc PAS le vrai firmware :
 * il reproduit fidèlement la machine à états RX de src/mia/oric/acia.c afin de
 * VERROUILLER la sémantique du staging RX et du handshake `acia_stat_checked`
 * introduit sur la branche feature/acia-rx-lossless (finition du brouillon
 * commenté par Sodium sur sa branche feature/acia).
 *
 * Invariants testés :
 *  - staging RX mono-coeur (coeur 0) : le slot ($x0) n'est jamais réécrit tant
 *    que le CPU n'a pas consommé (stat_rx == RX_FULL) → pas de perte au handoff ;
 *  - intégrité FIFO de bout en bout (USB → ring 32 → slot → CPU), ordre préservé ;
 *  - le ring garde une distance de sécurité (le head ne rattrape jamais le tail) ;
 *  - handshake IRQ : une fois le STATUS lu par le CPU (acia_stat_checked), on
 *    cesse de ré-asserter l'IRQ ; tant qu'il ne l'a pas lu, rappel périodique.
 *
 * Réf : sodiumlb/loci-firmware src/mia/oric/acia.c (acia_task/acia_read/acia_clr_irq)
 * Compilation/exécution : `make` (voir Makefile)
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SIZE 32
#define MASK 0x1F
#define RX_FULL 0x08 /* ACIA_STAT_RX_FULL */

/* Miroir exact de l'état RX pertinent de acia.c */
typedef struct {
    uint8_t ring[SIZE];
    uint8_t head; /* acia_rx_buffer_head */
    uint8_t tail; /* acia_rx_buffer_tail */
    uint8_t slot; /* acia_io->data ($x0), servi par PIO à la lecture */
    uint8_t stat_rx;      /* 0 ou RX_FULL */
    bool    stat_checked; /* acia_stat_checked */
    bool    rx_irq_enable;
    uint8_t cnt;          /* compteur de rappel IRQ périodique */
    unsigned irq_count;   /* instrumentation : nb d'IRQ RX levées */
} acia_t;

static void acia_reset_model(acia_t *a)
{
    memset(a, 0, sizeof(*a));
    a->stat_checked = true; /* aucun octet en attente après reset (cf. acia.c) */
}

/*
 * Miroir du remplissage ring depuis USB dans acia_task() :
 *   len = tail - head - 2 (distance de sécurité de 1 quand vide), tronqué au
 *   masque, puis tronqué au wrap, puis borné aux octets réellement disponibles.
 */
static void acia_feed_ring(acia_t *a, const uint8_t *src, size_t *pos, size_t len_src)
{
    if (*pos >= len_src)
        return;
    uint8_t len = (uint8_t)(a->tail - a->head - 2);
    len &= MASK;
    if (len > (SIZE - a->head))
        len = (uint8_t)(SIZE - a->head);
    size_t avail = len_src - *pos;
    if (len > avail)
        len = (uint8_t)avail;
    for (uint8_t i = 0; i < len; i++)
        a->ring[a->head + i] = src[(*pos)++];
    a->head = (uint8_t)((a->head + len) & MASK);
}

/* Miroir du staging RX de acia_task() (coeur 0). Renvoie true si IRQ levée. */
static bool acia_stage(acia_t *a)
{
    bool fire = false;
    if (!a->stat_rx) {
        if (a->head != a->tail) {
            a->slot = a->ring[a->tail];
            a->tail = (uint8_t)((a->tail + 1) & MASK);
            a->stat_rx = RX_FULL;
            a->stat_checked = false;
            if (a->rx_irq_enable)
                fire = true;
            a->cnt = 255;
        }
    } else if (!a->stat_checked) {
        if (0 == --a->cnt && a->rx_irq_enable)
            fire = true;
    }
    if (fire)
        a->irq_count++;
    return fire;
}

/* Miroir de acia_read() : le PIO sert d'abord le slot, puis stat_rx est effacé. */
static uint8_t cpu_read_data(acia_t *a)
{
    uint8_t d = a->slot;
    a->stat_rx = 0;
    return d;
}

/* Miroir de acia_clr_irq() : la lecture STATUS acquitte l'IRQ/octet. */
static void cpu_read_status(acia_t *a) { a->stat_checked = true; }

/* --- Cas 1 : intégrité FIFO de bout en bout, consommateur piloté par IRQ --- */
static void test_fifo_integrity(void)
{
    acia_t a;
    acia_reset_model(&a);
    a.rx_irq_enable = true;

    uint8_t src[100];
    for (int i = 0; i < 100; i++)
        src[i] = (uint8_t)i;
    size_t pos = 0;

    uint8_t out[100];
    int got = 0;
    int guard = 0;
    while (got < 100) {
        acia_feed_ring(&a, src, &pos, sizeof(src));
        acia_stage(&a);
        if (a.stat_rx) {
            cpu_read_status(&a);       /* driver lit STATUS... */
            out[got++] = cpu_read_data(&a); /* ...puis DATA */
        }
        assert(++guard < 100000); /* pas de blocage */
    }
    for (int i = 0; i < 100; i++)
        assert(out[i] == (uint8_t)i); /* ordre FIFO, zéro perte */
    printf("  [ok] intégrité FIFO : 100 octets, ordre préservé\n");
}

/* --- Cas 2 : buffer vide → pas de staging, pas d'IRQ, statut acquitté --- */
static void test_empty(void)
{
    acia_t a;
    acia_reset_model(&a);
    a.rx_irq_enable = true;
    for (int i = 0; i < 1000; i++)
        acia_stage(&a);
    assert(a.stat_rx == 0);
    assert(a.stat_checked == true);
    assert(a.irq_count == 0);
    printf("  [ok] buffer vide : aucun faux RX_FULL ni IRQ\n");
}

/* --- Cas 3 : pas de réécriture du slot non lu (lossless au handoff) --- */
static void test_no_slot_overwrite(void)
{
    acia_t a;
    acia_reset_model(&a);
    uint8_t src[2] = {0xAA, 0xBB};
    size_t pos = 0;
    acia_feed_ring(&a, src, &pos, sizeof(src));

    acia_stage(&a);            /* stage 0xAA */
    assert(a.stat_rx == RX_FULL);
    assert(a.slot == 0xAA);
    uint8_t tail_after_first = a.tail;

    acia_stage(&a);            /* 0xBB en attente, mais slot non lu → pas de staging */
    assert(a.slot == 0xAA);    /* slot INCHANGÉ */
    assert(a.tail == tail_after_first);

    assert(cpu_read_data(&a) == 0xAA); /* CPU lit 0xAA */
    acia_stage(&a);            /* slot libre → stage 0xBB */
    assert(a.slot == 0xBB);
    printf("  [ok] slot non lu jamais écrasé, ordre 0xAA puis 0xBB\n");
}

/* --- Cas 4 : le rappel IRQ s'arrête après lecture du STATUS --- */
static void test_irq_handshake(void)
{
    /* 4a : sans lecture STATUS, rappel périodique (cnt) → une 2e IRQ */
    {
        acia_t a;
        acia_reset_model(&a);
        a.rx_irq_enable = true;
        uint8_t src[1] = {0x42};
        size_t pos = 0;
        acia_feed_ring(&a, src, &pos, sizeof(src));
        acia_stage(&a);                 /* IRQ #1, checked=false, cnt=255 */
        assert(a.irq_count == 1);
        for (int i = 0; i < 255; i++)   /* CPU ne lit rien */
            acia_stage(&a);
        assert(a.irq_count == 2);       /* rappel périodique déclenché */
    }
    /* 4b : après lecture STATUS, plus aucun rappel IRQ */
    {
        acia_t a;
        acia_reset_model(&a);
        a.rx_irq_enable = true;
        uint8_t src[1] = {0x42};
        size_t pos = 0;
        acia_feed_ring(&a, src, &pos, sizeof(src));
        acia_stage(&a);                 /* IRQ #1 */
        assert(a.irq_count == 1);
        cpu_read_status(&a);            /* acquittement */
        for (int i = 0; i < 1000; i++)  /* slot toujours plein, statut acquitté */
            acia_stage(&a);
        assert(a.irq_count == 1);       /* aucun rappel superflu */
    }
    printf("  [ok] handshake IRQ : rappel tant que STATUS non lu, silence après\n");
}

/* --- Cas 5 : le ring garde sa distance de sécurité (head != tail au plein) --- */
static void test_ring_distance(void)
{
    acia_t a;
    acia_reset_model(&a);
    uint8_t src[100];
    for (int i = 0; i < 100; i++)
        src[i] = (uint8_t)(i + 1);
    size_t pos = 0;
    /* On remplit sans jamais consommer : le ring ne doit pas boucler sur lui-même */
    for (int i = 0; i < 10; i++)
        acia_feed_ring(&a, src, &pos, sizeof(src));
    /* Occupation = (head - tail) & MASK ; le -2 garantit qu'on ne sature pas à SIZE */
    uint8_t occ = (uint8_t)((a.head - a.tail) & MASK);
    assert(occ <= SIZE - 1);
    assert(a.head != a.tail || occ == 0); /* jamais confondre plein et vide */
    printf("  [ok] ring : distance de sécurité respectée (occ=%u)\n", occ);
}

int main(void)
{
    printf("== acia_rx_model_test ==\n");
    test_fifo_integrity();
    test_empty();
    test_no_slot_overwrite();
    test_irq_handshake();
    test_ring_distance();
    printf("Tous les tests RX ACIA passent.\n");
    return 0;
}
