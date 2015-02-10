#include "misc.h"
#include "tsdu.h"
#include "tpdu.h"
#include "misc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct _tpdu_ui_t {
    tsdu_t *tsdu;   ///< contains last decoded TSDU
};

tpdu_ui_t *tpdu_ui_create(void)
{
    tpdu_ui_t *tpdu = malloc(sizeof(tpdu_ui_t));
    if (!tpdu) {
        return NULL;
    }
    memset(tpdu, 0, sizeof(tpdu_ui_t));

    return tpdu;
}

void tpdu_ui_destroy(tpdu_ui_t *tpdu)
{
    tsdu_destroy(tpdu->tsdu);
    free(tpdu);
}

bool tpdu_ui_push_hdlc_frame(tpdu_ui_t *tpdu, const hdlc_frame_t *hdlc_fr)
{
    if (hdlc_fr->info_nbits < 8) {
        printf("WTF too short HDLC (%d)\n", hdlc_fr->info_nbits);
        return false;
    }

    bool ext = get_bits(1, 0, hdlc_fr->info);
    const bool seg = get_bits(1, 1, hdlc_fr->info);
    const uint8_t prio = get_bits(2, 2, hdlc_fr->info);
    const uint8_t id_tsap = get_bits(4, 4, hdlc_fr->info);

    printf("\tDU EXT=%d SEG=%d PRIO=%d ID_TSAP=%d", ext, seg, prio, id_tsap);
    if (ext == 0 && seg == 0) {
        tsdu_destroy(tpdu->tsdu);

        printf("\n");
        // PAS 0001-3-3 9.5.1.2
        // nbits > (3 * 8) for data frames (8 - ADDR - CMD - FCS) * 8
        // nbits > (7 * 8 + 4) for high rate data (11.5 - ADDR - CMD - FCS) * 8
        // but for 2 block lenght data frame nbist = (16 - ADDR - CMD - FCS)
        // thus data frame and high rate data can be distinguished by size
        if (hdlc_fr->info_nbits > (7 * 8 + 4)) {
            const int nbits = get_bits(8, 8, hdlc_fr->info) * 8;
            tpdu->tsdu = tsdu_d_decode(hdlc_fr->info + 2, nbits, prio, id_tsap);
        } else {
            const int nbits = hdlc_fr->info_nbits - 8;
            tpdu->tsdu = tsdu_d_decode(hdlc_fr->info + 1, nbits, prio, id_tsap);
        }
        return true;
    }

    if (ext != 1) {
        printf("\nWTF, unsupported ext and seg combination\n");
        return false;
    }

    ext = get_bits(1, 8, hdlc_fr->info);
    if (!ext) {
        printf("\nWTF unsupported short ext\n");
        return false;
    }

    uint8_t seg_ref = get_bits(7, 9, hdlc_fr->info);
    ext = get_bits(1, 16, hdlc_fr->info);
    if (ext != 0) {
        printf("\nWTF unsupported long ext\n");
        return false;
    }

    const bool res = get_bits(1, 17, hdlc_fr->info);
    const uint8_t packet_num = get_bits(6, 18, hdlc_fr->info);
    if (res) {
        printf("WTF res != 0\n");
    }
    printf(" SEGM_REF=%d, PACKET_NUM=%d\n", seg_ref, packet_num);

    // TODO

    return false;
}

tsdu_t *tpdu_ui_get_tsdu(tpdu_ui_t *tpdu)
{
    tsdu_t *tsdu = tpdu->tsdu;
    tpdu->tsdu = NULL;
    return tsdu;
}


uint8_t segbuf[10000];
int numoctets, startmod;

void segmentation_reset(void) {
    numoctets=0;
}

static void tpdu_du_process(const uint8_t* t, int length, int mod) {

    int ext, seg, prio, id_tsap;
    int data_length=0;
    int segmentation_reference, packet_number;

    ext=bits_to_int(t, 1);
    seg=bits_to_int(t+1, 1);
    prio=bits_to_int(t+2, 2);
    id_tsap=bits_to_int(t+4, 4);

    if (ext==0) {		// No segmentation

        printf("\tDU EXT=%i SEG=%i PRIO=%i ID_TSAP=%i DATA_LENGTH=%i\n", ext, seg, prio, id_tsap, length);

        if (length > 3) {
            data_length=bits_to_int(t+8, 8);
            tsdu_process(t+16, data_length, mod);
        } else
            tsdu_process(t+8, length-1, mod);

    } else {		// Segmentation

        segmentation_reference=bits_to_int(t+9, 7);
        packet_number=bits_to_int(t+18, 6);

        printf("\tDU EXT=%i SEG=%i PRIO=%i ID_TSAP=%i SEGM_REF=%i, PACKET_NUM=%i\n", ext, seg, prio, id_tsap, segmentation_reference, packet_number);

        if (seg==1) {
            memcpy(segbuf+numoctets*8, t+24, (length-3)*8);
            numoctets = numoctets + length-3;
        }
        else {		// Last segment
            data_length=bits_to_int(t+24, 8);
            memcpy(segbuf+numoctets*8, t+32, data_length*8);
            numoctets = numoctets + data_length;
            printf("multiseg %i\n", numoctets);
            print_buf(segbuf, numoctets*8);
            tsdu_process(segbuf, numoctets, startmod);
            numoctets=0;

        }

    }



}

static void tpdu_i_process(const uint8_t* t, int length, int mod) {

    int ext, seg, d, tpdu_code;
    int par_field, dest_ref;
    int data_length;

    ext=bits_to_int(t, 1);
    seg=bits_to_int(t+1, 1);
    d=bits_to_int(t+2, 1);
    tpdu_code=bits_to_int(t+3, 5);

    par_field=bits_to_int(t+8, 4);
    dest_ref=bits_to_int(t+12, 4);

    if ((tpdu_code & 0x18) == 0) {
        printf("\tI CR EXT=%i SEG=%i D=%i TPDU_CODE=%i PAR_FIELD=%i DEST_REF=%i\n", ext, seg, d, tpdu_code, par_field, dest_ref);
    } else if ((tpdu_code & 0x18)  == 8) {
        printf("\tI CC EXT=%i SEG=%i D=%i TPDU_CODE=%i PAR_FIELD=%i DEST_REF=%i\n", ext, seg, d, tpdu_code, par_field, dest_ref);
    } else if ((tpdu_code & 0x18) == 16) {
        printf("\tI FCR EXT=%i SEG=%i D=%i TPDU_CODE=%i PAR_FIELD=%i DEST_REF=%i\n", ext, seg, d, tpdu_code, par_field, dest_ref);
    } else if (tpdu_code == 24) {
        printf("\tI DR EXT=%i SEG=%i D=%i TPDU_CODE=%i PAR_FIELD=%i DEST_REF=%i\n", ext, seg, d, tpdu_code, par_field, dest_ref);
    } else if (tpdu_code == 25) {
        printf("\tI FDR EXT=%i SEG=%i D=%i TPDU_CODE=%i PAR_FIELD=%i DEST_REF=%i\n", ext, seg, d, tpdu_code, par_field, dest_ref);
    } else if (tpdu_code == 26) {
        printf("\tI DC EXT=%i SEG=%i D=%i TPDU_CODE=%i PAR_FIELD=%i DEST_REF=%i\n", ext, seg, d, tpdu_code, par_field, dest_ref);
    } else if (tpdu_code == 27) {
        printf("\tI DT EXT=%i SEG=%i D=%i TPDU_CODE=%i PAR_FIELD=%i DEST_REF=%i\n", ext, seg, d, tpdu_code, par_field, dest_ref);
    } else if (tpdu_code == 28) {
        printf("\tI DTE EXT=%i SEG=%i D=%i TPDU_CODE=%i PAR_FIELD=%i DEST_REF=%i\n", ext, seg, d, tpdu_code, par_field, dest_ref);
    } else {
        printf("\tI xxx EXT=%i SEG=%i D=%i TPDU_CODE=%i PAR_FIELD=%i DEST_REF=%i\n", ext, seg, d, tpdu_code, par_field, dest_ref);
    }

    if ((d==1) && (seg==0)) {
        data_length=bits_to_int(t+16, 8);
        tsdu_process(t+24, data_length, mod);
    }

    //TODO: segmentation

}

static void hdlc_process(const uint8_t *t, int length, int mod) {

    int hdlc, r, s, m;

    hdlc = bits_to_int(t, 8);

    if ((hdlc & 0x01) == 0) {
        r = (hdlc & 0xe0) >> 5;
        s = (hdlc & 0x0e) >> 1;
        //pe = (hdlc & 0x10) >> 4;
        printf("\tHDLC I(%i,%i)\n", r, s);
        tpdu_i_process(t+8, length-1, mod);
    } else if ((hdlc & 0x0f) == 13) {
        r = (hdlc & 0xe0) >> 5;
        printf("\tHDLC A(%i) ACK_DACH\n", r);
    } else if ((hdlc & 0x03) == 1) {
        r = (hdlc & 0xe0) >> 5;
        s = (hdlc & 0x0c) >> 2;
        printf("\tHDLC S(%i) ", r);
        switch(s) {
            case 0:
                printf("RR\n");
                break;
            case 1:
                printf("RNR\n");
                break;
            case 2:
                printf("REJ\n");
                break;
        }
    } else if ((hdlc & 0x03) == 3) {
        m = ((hdlc & 0xe0) >> 3) + ((hdlc & 0x0c) >> 2);
        printf("\tHDLC UI ");
        switch(m) {
            case 0:
                printf("UI\n");
                break;
            case 8:
                printf("DISC\n");
                break;
            case 12:
                printf("UA\n");
                break;
            case 16:
                printf("SNRM\n");
                break;
            case 20:
                printf("UI_CD\n");
                break;
            case 24:
                printf("UI_VCH\n");
                break;
            default: 
                printf("unknown\n");
                break;

        }
        tpdu_du_process(t+8, length-1, mod);
    } else {

        printf("\tHDLC xxx\n");
    }
}

void tpdu_process(const uint8_t* t, int length, int *frame_no) {

    printf("\tADDR=");
    decode_addr(t);

    hdlc_process(t+16,length-2, *frame_no);
}

