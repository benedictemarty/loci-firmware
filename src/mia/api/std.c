/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include "api/api.h"
#include "api/std.h"
#include "api/net.h"
#include "sys/com.h"
#include "sys/cpu.h"
#include "sys/mia.h"
//#include "sys/pix.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "fatfs/ff.h"
#include "sys/lfs.h"
#include "hardware/flash.h"
#include <stdio.h>

/* loci-webdisk : restauré à 16 (valeur upstream) après portage de la Stratégie C.
 * Le workaround précédent (16->10) contournait le débordement RAM du binaire
 * copy_to_ram ; la Stratégie C (binaire FLASH/XIP) libère ~148 Ko et rend la
 * réduction inutile — on retrouve les 16 fichiers FAT ouverts simultanément. */
#define STD_FIL_MAX 16
FIL std_fil[STD_FIL_MAX];
#define STD_LFS_MAX 2
bool lfs_isopen[STD_LFS_MAX] = {false};
lfs_file_t lfs_fil[STD_LFS_MAX];

#define STD_FIL_STDIN 0
#define STD_FIL_STDOUT 1
#define STD_FIL_STDERR 2
#define STD_FIL_OFFS 3
#define STD_LFS_OFFS (STD_FIL_OFFS+STD_FIL_MAX)
/* Device reseau `N:` ($B7) : ses descripteurs suivent ceux du littlefs. Le canal
 * AT du modem etant unique, il n'y en a qu'un (NET_FD_MAX). */
#define STD_NET_OFFS (STD_LFS_OFFS+STD_LFS_MAX)
#define STD_FD_END   (STD_NET_OFFS+NET_FD_MAX)
static_assert(STD_FD_END < 128);

static int32_t std_xram_count = -1;
static int32_t std_in_count = -1;

static volatile size_t std_out_tail;
static volatile size_t std_out_head;
static volatile uint8_t std_out_buf[32];
#define STD_OUT_BUF(pos) std_out_buf[(pos) & 0x1F]

static inline bool std_out_buf_writable()
{
    return (((std_out_head + 1) & 0x1F) != (std_out_tail & 0x1F));
}

static inline void std_out_buf_write(char ch)
{
    STD_OUT_BUF(++std_out_head) = ch;
}

bool std_active(void)
{
    // Active until stdout is empty
    return (((std_out_head) & 0x1F) != (std_out_tail & 0x1F));
}

void std_task(void)
{
    // 6502 applications write to std_out_buf which we route
    // through the Pi Pico STDIO driver for CR/LR translation.
    if ((&STD_OUT_BUF(std_out_head) != &STD_OUT_BUF(std_out_tail)) &&
        (&COM_TX_BUF(com_tx_head + 1) != &COM_TX_BUF(com_tx_tail)) &&
        (&COM_TX_BUF(com_tx_head + 2) != &COM_TX_BUF(com_tx_tail)))
        putchar(STD_OUT_BUF(++std_out_tail));
}

/* Lecture réseau en cours : le device `N:` rend la main SANS répondre tant que
 * rien n'est prêt (BUSY maintenu), et `api_task` rappelle alors `main_api` avec le
 * MÊME opcode. Les paramètres ayant déjà été dépilés au premier passage, il faut
 * les mémoriser : sans ça le second passage redépile un xstack vide et rend
 * EINVAL. Constaté depuis un vrai programme 6502 (`open` réussi, `read` en
 * EINVAL alors que la transaction était bien en réception) — le chemin C direct
 * des tests ne passait pas par `api_task` et ne pouvait pas le montrer. */
static bool     net_rd_active;
static uint16_t net_rd_addr;
static uint16_t net_rd_count;

void std_api_open(void)
{
    // These match CC65 which is closer to POSIX than FatFs.
    const unsigned char RDWR = 0x03;
    const unsigned char CREAT = 0x10;
    const unsigned char TRUNC = 0x20;
    const unsigned char APPEND = 0x40;
    const unsigned char EXCL = 0x80;

    uint8_t flags = API_A;
    uint16_t mode = flags & RDWR; // RDWR are same bits
    uint8_t *path = &xstack[xstack_ptr];
    api_zxstack();
    if (net_is_path(path)) {            /* device reseau N: -> backend $B7 */
        int e = net_open(path, flags);
        if (e)
            return api_return_errno(e);
        return api_return_ax(STD_NET_OFFS);
    }
    if(path[0]=='0' && path[1]==':'){   //Internal flash at 0:
        path = &path[2];
        if (flags & CREAT)
        {
            if (flags & EXCL)
                mode |= LFS_O_EXCL;
            else
            {
                if (flags & TRUNC)
                    mode |= LFS_O_TRUNC;
                else if (flags & APPEND)
                    mode |= LFS_O_APPEND;
                else
                    mode |= LFS_O_CREAT;
            }
        }

        int fd = 0;
        for(; fd < STD_LFS_MAX; fd++)
            if (!lfs_isopen[fd])
                break;
        if (fd == STD_LFS_MAX)
            return api_return_errno(API_EMFILE);
        lfs_file_t *fp = &lfs_fil[fd];
        int lfsresult = lfs_file_opencfg(&lfs_volume, fp, (char*)path, mode, lfs_alloc_file_config());
        if(lfsresult < 0)
            return (api_return_errno(API_ELFSFS(lfsresult)));
        lfs_isopen[fd] = true;
        return api_return_ax(fd + STD_LFS_OFFS);
    }else{
        if (flags & CREAT)
        {
            if (flags & EXCL)
                mode |= FA_CREATE_NEW;
            else
            {
                if (flags & TRUNC)
                    mode |= FA_CREATE_ALWAYS;
                else if (flags & APPEND)
                    mode |= FA_OPEN_APPEND;
                else
                    mode |= FA_OPEN_ALWAYS;
            }
        }

        int fd = 0;
        for (; fd < STD_FIL_MAX; fd++)
            if (!std_fil[fd].obj.fs)
                break;
        if (fd == STD_FIL_MAX)
            return api_return_errno(API_EMFILE);
        FIL *fp = &std_fil[fd];
        FRESULT fresult = f_open(fp, (TCHAR *)path, mode);
        if (fresult != FR_OK)
            return api_return_errno(API_EFATFS(fresult));
        return api_return_ax(fd + STD_FIL_OFFS);
    }
}

void std_api_close(void)
{
    int fd = API_A;
    if (fd < STD_FIL_OFFS || fd >= STD_FD_END)
        return api_return_errno(API_EINVAL);
    if (fd >= STD_NET_OFFS){
        net_rd_active = false;      /* annule une lecture restee en attente */
        net_close();
        return api_return_ax(0);
    }
    if (fd >= STD_LFS_OFFS){
        lfs_file_t *fp = &lfs_fil[fd-STD_LFS_OFFS];
        lfs_free_file_config(fp);
        int lfsresult = lfs_file_close(&lfs_volume, fp);
        if (lfsresult < 0)
            return api_return_ax(API_ELFSFS(lfsresult));
        lfs_isopen[fd-STD_LFS_OFFS] = false;
        return api_return_ax(0);
    }else{
        FIL *fp = &std_fil[fd - STD_FIL_OFFS];
        FRESULT fresult = f_close(fp);
        if (fresult != FR_OK)
            return api_return_errno(API_EFATFS(fresult));
        return api_return_ax(0);
    }
}

void std_api_read_xstack(void)
{
    uint8_t *buf;
    uint16_t count;
    UINT br;
    if (std_in_count >= 0)
    {
        if (!cpu_stdin_ready())
            return;
        count = std_in_count;
        std_in_count = -1;
        buf = &xstack[XSTACK_SIZE - count];
        br = cpu_stdin_read(buf, count);
    }
    else
    {
        int16_t fd = API_A;
        if (!api_pop_uint16_end(&count) ||
            (fd && fd < STD_FIL_OFFS) ||
            fd >= STD_LFS_MAX + STD_LFS_OFFS ||
            count > XSTACK_SIZE)
            return api_return_errno(API_EINVAL);
        buf = &xstack[XSTACK_SIZE - count];
        if (!fd)
        {
            std_in_count = count;
            cpu_stdin_request();
            return;
        }
        if(fd >= STD_LFS_OFFS){
            lfs_file_t *fp = &lfs_fil[fd - STD_LFS_OFFS];
            int lfsresult = lfs_file_read(&lfs_volume, fp, buf, count);
            if (lfsresult < 0){
                API_ERRNO = API_ELFSFS(lfsresult);
                api_set_ax(-1);
            }
            br = lfsresult;
        }else{
            FIL *fp = &std_fil[fd - STD_FIL_OFFS];
            FRESULT fresult = f_read(fp, buf, count, &br);
            if (fresult != FR_OK)
            {
                API_ERRNO = fresult;
                api_set_ax(-1);
                return;
            }
        }
    }
    api_set_ax(br);
    xstack_ptr = XSTACK_SIZE;
    if (br == count)
        xstack_ptr -= count;
    else // relocate short read
        for (UINT i = br; i;)
            xstack[--xstack_ptr] = buf[--i];
    api_sync_xstack();
    api_return_released();
}

void std_api_read_xram(void)
{
    static uint16_t xram_addr;
    if (net_rd_active) {
        int32_t n = net_read((uint8_t *)&xram[net_rd_addr], net_rd_count);
        if (n == -1)
            return;                     /* toujours rien : BUSY reste posé */
        net_rd_active = false;
        if (n == -2)
            return api_return_errno(net_errno());
        api_set_ax((uint16_t)n);
        std_xram_count = n;
        return;
    }
    if (std_in_count >= 0)
    {
        if (!cpu_stdin_ready())
            return;
        std_xram_count = cpu_stdin_read(&xram[xram_addr], std_in_count);
        std_in_count = -1;
    }
    if (std_xram_count >= 0)
    {
        for (; std_xram_count
         //&& pix_ready()
         ; --std_xram_count, ++xram_addr)
         {}
         //   pix_send(PIX_DEVICE_XRAM, 0, xram[xram_addr], xram_addr);
        if (!std_xram_count)
        {
            std_xram_count = -1;
            api_return_released();
        }
        return;
    }
    uint8_t *buf;
    uint16_t count;
    int16_t fd = API_A;
    UINT br;
    if (!api_pop_uint16(&count) ||
        !api_pop_uint16_end(&xram_addr) ||
        (fd && fd < STD_FIL_OFFS) ||
        fd >= STD_FD_END)
        return api_return_errno(API_EINVAL);
    if (fd >= STD_NET_OFFS) {
        /* Device reseau : machine a etats. Tant que rien n'est pret, on RETOURNE
         * SANS repondre (BUSY reste pose) et le 6502 rappelle au tour suivant —
         * patron socle §5.3. net_task() remplit l'anneau pendant ce temps.
         * Les parametres sont MEMORISES ici : au rappel, le xstack est vide et il
         * ne faut surtout pas le redepiler (cf. net_rd_active en tete). */
        buf = &xram[xram_addr];
        if (count > 0x7FFF)
            count = 0x7FFF;
        if (buf + count > xram + 0x10000)
            return api_return_errno(API_EINVAL);
        int32_t n = net_read(buf, count);
        if (n == -1) {
            net_rd_active = true;
            net_rd_addr = xram_addr;
            net_rd_count = count;
            return;                     /* rien de pret : ne pas repondre */
        }
        if (n == -2)
            return api_return_errno(net_errno());
        api_set_ax((uint16_t)n);
        std_xram_count = n;             /* la boucle ci-dessus finit le travail */
        return;
    }
    if (!fd)
    {
        cpu_stdin_request();
        std_in_count = std_xram_count = count;
        return;
    }
    buf = &xram[xram_addr];
    if (count > 0x7FFF)
        count = 0x7FFF;
    if (buf + count > xram + 0x10000)
        return api_return_errno(API_EINVAL);
    if( fd >= STD_LFS_OFFS){
        lfs_file_t *fp = &lfs_fil[fd - STD_LFS_OFFS];
        int lfsresult = lfs_file_read(&lfs_volume, fp, buf, count);
        if (lfsresult < 0){
            API_ERRNO = API_ELFSFS(lfsresult);
            api_set_ax(-1);
        }else{
            api_set_ax(lfsresult);
            std_xram_count = lfsresult;
        }
    }else{
        FIL *fp = &std_fil[fd - STD_FIL_OFFS];
        FRESULT fresult = f_read(fp, buf, count, &br);
        if (fresult == FR_OK)
            api_set_ax(br);
        else
        {
            API_ERRNO = fresult;
            api_set_ax(-1);
        }
        std_xram_count = br;
    }
}

// Non-blocking write
static void std_out_write(char *ptr)
{
    static void *std_out_ptr;
    if (ptr)
        std_out_ptr = ptr;
    for (; std_xram_count && std_out_buf_writable(); --std_xram_count)
        std_out_buf_write(*(uint8_t *)std_out_ptr++);
    if (!std_xram_count)
    {
        std_xram_count = -1;
        api_return_released();
    }
}

void std_api_write_xstack(void)
{
    if (std_xram_count >= 0)
        return std_out_write(NULL);
    uint8_t *buf;
    uint16_t count;
    int fd = API_A;
    if (fd == STD_FIL_STDIN || fd >= STD_LFS_MAX + STD_LFS_OFFS)
        return api_return_errno(API_EINVAL);
    count = XSTACK_SIZE - xstack_ptr;
    buf = &xstack[xstack_ptr];
    api_zxstack();
    if (fd < STD_FIL_OFFS)
    {
        api_set_ax(count);
        std_xram_count = count;
        std_out_write((char *)buf);
        return;
    }
    if (fd >= STD_LFS_OFFS){
        lfs_file_t *fp = &lfs_fil[fd - STD_LFS_OFFS];
        int lfsresult = lfs_file_write(&lfs_volume, fp, buf, count);
        if(lfsresult < 0)
            return api_return_errno(API_ELFSFS(lfsresult));
        return api_return_ax(lfsresult);
    }else{
        FIL *fp = &std_fil[fd - STD_FIL_OFFS];
        UINT bw;
        FRESULT fresult = f_write(fp, buf, count, &bw);
        if (fresult != FR_OK)
            return api_return_errno(API_EFATFS(fresult));
        return api_return_ax(bw);
    }
}

void std_api_write_xram(void)
{
    if (std_xram_count >= 0)
        return std_out_write(NULL);
    uint8_t *buf;
    uint16_t xram_addr;
    uint16_t count;
    int fd = API_A;
    if (fd == STD_FIL_STDIN || fd >= STD_LFS_MAX + STD_LFS_OFFS)
        return api_return_errno(API_EINVAL);
    if (!api_pop_uint16(&count) ||
        !api_pop_uint16_end(&xram_addr))
        return api_return_errno(API_EINVAL);
    buf = &xram[xram_addr];
    if (buf + count > xram + 0x10000)
        return api_return_errno(API_EINVAL);
    if (count > 0x7FFF)
        count = 0x7FFF;
    if (fd < STD_FIL_OFFS)
    {
        api_set_ax(count);
        std_xram_count = count;
        std_out_write((char *)buf);
        return;
    }
    if (fd >= STD_LFS_OFFS){
        lfs_file_t *fp = &lfs_fil[fd - STD_LFS_OFFS];
        int lfsresult = lfs_file_write(&lfs_volume, fp, buf, count);
        if(lfsresult < 0)
            return api_return_errno(API_ELFSFS(lfsresult));
        return api_return_ax(lfsresult);
    }else{
        FIL *fp = &std_fil[fd - STD_FIL_OFFS];
        UINT bw;
        FRESULT fresult = f_write(fp, buf, count, &bw);
        if (fresult != FR_OK)
            return api_return_errno(API_EFATFS(fresult));
        return api_return_ax(bw);
    }
}

void std_api_lseek(void)
{
    int8_t whence;
    int32_t ofs;
    int fd = API_A;
    if (fd < STD_FIL_OFFS ||
        fd >= STD_LFS_MAX + STD_LFS_OFFS ||
        !api_pop_int8(&whence) ||
        !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    if (fd >= STD_LFS_OFFS){
        lfs_file_t *fp = &lfs_fil[fd - STD_LFS_OFFS];
        switch (whence)
        {
        case 0: // SEEK_CUR
            whence = LFS_SEEK_CUR;
            break;
        case 1: // SEEK_END
            whence = LFS_SEEK_END;
            break;
        case 2: // SEEK_SET
            whence = LFS_SEEK_SET;
            break;
        default:
            return api_return_errno(API_EINVAL);
        }
        int lfsresult = lfs_file_seek(&lfs_volume,fp,ofs,whence);
        if (lfsresult < 0)
            return api_return_errno(API_ELFSFS(lfsresult));
        return api_return_axsreg(lfsresult);
    }else{
        FIL *fp = &std_fil[fd - STD_FIL_OFFS];
        switch (whence) // CC65
        {
        case 0: // SEEK_CUR
            ofs += f_tell(fp);
            break;
        case 1: // SEEK_END
            ofs += f_size(fp);
            break;
        case 2: // SEEK_SET
            break;
        default:
            return api_return_errno(API_EINVAL);
        }
        FRESULT fresult = f_lseek(fp, ofs);
        if (fresult != FR_OK)
            return api_return_errno(API_EFATFS(fresult));
        FSIZE_t pos = f_tell(fp);
        // Beyond 2GB is darkness.
        if (pos > 0x7FFFFFFF)
            pos = 0x7FFFFFFF;
        return api_return_axsreg(pos);
    }
}

/*
 * MIA_OP_STREAM_BANK ($A8) — streamer read-only fichier -> banque 16 Ko.
 *
 * Un seul fastcall = lseek(SEEK_SET) + read dans la zone XRAM de la banque SEL,
 * puis (option MAP) mapping de la banque en $C000-$FFFF via le meme chemin que
 * $A7 (map_api_set_bank -> mia_set_bank, cote coeur 0).
 *
 * Argument (registre A) : bit7 MAP, bits3:0 SEL (0..3).
 * xstack (pousse par l'appelant, depile ici en LIFO) :
 *   fd (int8)  off (int32, SEEK_SET)  dst (uint16, 0..0x3FFF)  len (uint16)
 * Retour (AX) : octets reellement lus, ou -1 + errno.
 *
 * Cf. extensions/streamer-A8/spec-streamer-assets.md (niveau 1, §4).
 */
void std_api_stream_bank(void)
{
    uint8_t a = API_A;
    uint8_t sel = a & 0x0F;
    bool map = !!(a & 0x80);
    uint16_t len, dst;
    int32_t off;
    int8_t fd8;
    // Depilage LIFO : dernier pousse (len) en premier ; fd (premier pousse) en _end.
    if (!api_pop_uint16(&len) ||
        !api_pop_uint16(&dst) ||
        !api_pop_int32(&off) ||
        !api_pop_int8_end(&fd8))
        return api_return_errno(API_EINVAL);
    int fd = fd8;
    // fichier reel requis (pas stdin), banque allouee 0..3, offset intra-banque.
    if (fd < STD_FIL_OFFS ||
        fd >= STD_LFS_MAX + STD_LFS_OFFS ||
        sel > 3 ||
        dst > 0x3FFF)
        return api_return_errno(API_EINVAL);
    // Bornage de la longueur a l'interieur de la banque 16 Ko.
    if (len > (uint16_t)(0x4000 - dst))
        len = (uint16_t)(0x4000 - dst);
    // Cible = xram[(SEL<<14) + dst] (base = 0x20000000 + SEL*0x4000, cf. mia_set_bank).
    uint8_t *buf = &xram[((uint32_t)sel << 14) + dst];
    if (buf + len > xram + 0x10000)
        return api_return_errno(API_EINVAL);
    // Lecture synchrone : seul blocage coeur 0 tolere (patron std_api_read_xram).
    int nread;
    if (fd >= STD_LFS_OFFS)
    {
        lfs_file_t *fp = &lfs_fil[fd - STD_LFS_OFFS];
        int r = lfs_file_seek(&lfs_volume, fp, off, LFS_SEEK_SET);
        if (r < 0)
            return api_return_errno(API_ELFSFS(r));
        nread = lfs_file_read(&lfs_volume, fp, buf, len);
        if (nread < 0)
            return api_return_errno(API_ELFSFS(nread));
    }
    else
    {
        FIL *fp = &std_fil[fd - STD_FIL_OFFS];
        FRESULT fr = f_lseek(fp, (FSIZE_t)off);
        if (fr != FR_OK)
            return api_return_errno(API_EFATFS(fr));
        UINT br;
        fr = f_read(fp, buf, len, &br);
        if (fr != FR_OK)
            return api_return_errno(API_EFATFS(fr));
        nread = (int)br;
    }
    __dmb(); // stores XRAM visibles avant tout mapping de banque
    if (map)
        mia_set_bank(sel, true); // meme chemin que $A7 (coeur 0)
    return api_return_ax((uint16_t)nread);
}

void std_api_unlink(void)
{
    uint8_t *path = &xstack[xstack_ptr];
    api_zxstack();
    if(path[0]=='0' && path[1]==':'){
        path = &path[2];
        int lfsresult = lfs_remove(&lfs_volume, (char*)path);
        if(lfsresult < 0)
            return api_return_errno(API_ELFSFS(lfsresult));
    }else{
        FRESULT fresult = f_unlink((TCHAR *)path);
        if (fresult != FR_OK)
            return api_return_errno(API_EFATFS(fresult));
    }
    return api_return_ax(0);
}

void std_api_rename(void)
{
    uint8_t *oldname, *newname;
    oldname = newname = &xstack[xstack_ptr];
    api_zxstack();
    while (*oldname)
        oldname++;
    if (oldname == &xstack[XSTACK_SIZE])
        return api_return_errno(API_EINVAL);
    oldname++;
    if(oldname[0]=='0' && oldname[1]==':'){
        oldname = &oldname[2];
        if(newname[0]=='0' && newname[1]==':')
            newname = &newname[2];
        else
            return api_return_errno(API_EINVAL);
        int lfsresult = lfs_rename(&lfs_volume, (char*)oldname, (char*)newname);
        if (lfsresult < 0)
            return api_return_errno(API_ELFSFS(lfsresult));
    }else{
        FRESULT fresult = f_rename((TCHAR *)oldname, (TCHAR *)newname);
        if (fresult != FR_OK)
            return api_return_errno(API_EFATFS(fresult));
    }
    return api_return_ax(0);
}

void std_stop(void)
{
    std_xram_count = -1;
    std_in_count = -1;
    for (int i = 0; i < STD_FIL_MAX; i++)
        if (std_fil[i].obj.fs)
            f_close(&std_fil[i]);
    for (int i = 0; i < STD_LFS_MAX; i++)
        if (lfs_isopen[i]){
            lfs_free_file_config(&lfs_fil[i]);
            lfs_file_close(&lfs_volume, &lfs_fil[i]);
        }
}

/* ── Complément POSIX (extensions/fs-posix, additif) ─────────────────────
 * Greffe de primitives FatFS/littlefs déjà liées, jamais exposées à l'API.
 * Opcodes : $1E SYNCFS, $1F STAT (ici) ; $84 CHDIR, $85 GETFREE (dir.c). */

/* $1E SYNCFS — A = fd (ou $FF = tous les fichiers ouverts). Rend 0 / errno. */
void std_api_syncfs(void)
{
    int fd = API_A;
    if (fd == 0xFF) {
        for (int i = 0; i < STD_FIL_MAX; i++)
            if (std_fil[i].obj.fs) {
                FRESULT fresult = f_sync(&std_fil[i]);
                if (fresult != FR_OK)
                    return api_return_errno(API_EFATFS(fresult));
            }
        for (int i = 0; i < STD_LFS_MAX; i++)
            if (lfs_isopen[i]) {
                int lfsresult = lfs_file_sync(&lfs_volume, &lfs_fil[i]);
                if (lfsresult < 0)
                    return api_return_errno(API_ELFSFS(lfsresult));
            }
        return api_return_ax(0);
    }
    if (fd < STD_FIL_OFFS || fd >= STD_FD_END)
        return api_return_errno(API_EINVAL);
    if (fd >= STD_NET_OFFS)
        return api_return_ax(0);            /* réseau : rien à synchroniser */
    if (fd >= STD_LFS_OFFS) {
        fd -= STD_LFS_OFFS;
        if (!lfs_isopen[fd])
            return api_return_errno(API_EINVAL);
        int lfsresult = lfs_file_sync(&lfs_volume, &lfs_fil[fd]);
        if (lfsresult < 0)
            return api_return_errno(API_ELFSFS(lfsresult));
        return api_return_ax(0);
    }
    fd -= STD_FIL_OFFS;
    if (!std_fil[fd].obj.fs)
        return api_return_errno(API_EINVAL);
    FRESULT fresult = f_sync(&std_fil[fd]);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    return api_return_ax(0);
}

/* $1F STAT — chemin sur la xstack. Même opcode et même ABI que RP6502 (RIA_OP_STAT,
 * cc65 libsrc/rp6502/f_stat.c) : AX = 0 / -1 (errno) et la structure f_stat_t est
 * poussée pour se DÉPILER dans l'ordre des champs :
 *   fsize (4) fdate (2) ftime (2) crdate (2) crtime (2) fattrib (1) altname[13] fname[256]
 * = 282 octets (xstack 512). LOCI : crdate/crtime = 0 (FF_FS_CRTIME absent) ;
 * littlefs (0:) : fattrib = AM_DIR ou 0, dates 0, altname vide, fname = nom. */
void std_api_stat(void)
{
    uint8_t *path = &xstack[xstack_ptr];
    api_zxstack();
    uint32_t fsize; uint16_t fdate, ftime, zero16 = 0; uint8_t fattrib;
    char altname[13] = {0};
    char fname[256] = {0};
    if (path[0] == '0' && path[1] == ':') {
        struct lfs_info info;
        int lfsresult = lfs_stat(&lfs_volume, (const char *)&path[2], &info);
        if (lfsresult < 0)
            return api_return_errno(API_ELFSFS(lfsresult));
        fattrib = (info.type == LFS_TYPE_DIR) ? AM_DIR : 0;
        fdate = 0; ftime = 0; fsize = info.size;
        strncpy(fname, info.name, sizeof fname - 1);
    } else {
        FILINFO fno;
        FRESULT fresult = f_stat((TCHAR *)path, &fno);
        if (fresult != FR_OK)
            return api_return_errno(API_EFATFS(fresult));
        fattrib = fno.fattrib; fdate = fno.fdate; ftime = fno.ftime; fsize = (uint32_t)fno.fsize;
        strncpy(altname, fno.altname, sizeof altname - 1);
        strncpy(fname, fno.fname, sizeof fname - 1);
    }
    /* Poussé à l'envers : le dernier poussé est le premier dépilé (fsize, octet bas). */
    api_push_n(fname, sizeof fname);
    api_push_n(altname, sizeof altname);
    api_push_uint8(&fattrib);
    api_push_uint16(&zero16);      /* crtime */
    api_push_uint16(&zero16);      /* crdate */
    api_push_uint16(&ftime);
    api_push_uint16(&fdate);
    api_push_uint32(&fsize);
    api_sync_xstack();
    return api_return_ax(0);
}
