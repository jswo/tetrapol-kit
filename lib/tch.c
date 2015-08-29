#define LOG_PREFIX "tch"
#include <tetrapol/tch.h>
#include <tetrapol/log.h>
#include <tetrapol/sdch.h>
#include <stdlib.h>

struct tch_priv_t {
    sdch_t *sdch;
};

tch_t *tch_create(void)
{
    tch_t *tch = malloc(sizeof(tch_t));
    if (!tch) {
        return NULL;
    }

    tch->sdch = sdch_create();
    if (!tch->sdch) {
        free(tch);
        return NULL;
    }

    return tch;
}

void tch_destroy(tch_t *tch)
{
    if (tch) {
        sdch_destroy(tch->sdch);
    }

    free(tch);
}

int tch_push_data_block(tch_t *tch, data_block_t *data_blk)
{
    const bool crc_ok = data_block_check_crc(data_blk);

    // TODO: separate VCH, SCH, SCH_TI

    if (data_blk->nerrs == 0 && crc_ok && (data_blk->fr_type == FRAME_TYPE_VOICE)) {
        LOG(INFO,"VOICE FRAME asb=%i", data_block_get_asb(data_blk).xy);

        return 0;
    }

    else if (data_blk->nerrs == 0 && crc_ok && (data_blk->fr_type == FRAME_TYPE_DATA)) {
        LOG(INFO,"DATA FRAME asb=%i", data_block_get_asb(data_blk).xy);

        if (sdch_dl_push_data_frame(tch->sdch, data_blk)) {
            tsdu_t *tsdu = sdch_get_tsdu(tch->sdch);
            if (tsdu) {
                LOG_IF(INFO) {
                    LOG_("\n");
                    tsdu_print(tsdu);
                }
            }
            tsdu_destroy(tsdu);

            return 0;
        }
    }

    return -1;
}

void tch_tick(const timeval_t *tv, void *tch_)
{
    tch_t *tch = tch_;
    sdch_tick(tv, tch->sdch);
}