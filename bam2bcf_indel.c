/*  bam2bcf_indel.c -- indel caller.

    Copyright (C) 2010, 2011 Broad Institute.
    Copyright (C) 2012-2014,2016 Genome Research Ltd.

    Author: Heng Li <lh3@sanger.ac.uk>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.  */

#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <htslib/hts.h>
#include <htslib/sam.h>
#include <htslib/khash_str2int.h>
#include "bam2bcf.h"
#include "str_finder.h"

#include <htslib/ksort.h>
KSORT_INIT_GENERIC(uint32_t)

#define MINUS_CONST 0x10000000
#define INDEL_WINDOW_SIZE 100

// Take a reference position tpos and convert to a query position (returned).
// This uses the CIGAR string plus alignment c->pos to do the mapping.
//
// *_tpos is returned as tpos if query overlaps tpos, but for deletions
// it'll be either the start (is_left) or end (!is_left) ref position.
static int tpos2qpos(const bam1_core_t *c, const uint32_t *cigar, int32_t tpos, int is_left, int32_t *_tpos)
{
    // x = pos in ref, y = pos in query seq
    int k, x = c->pos, y = 0, last_y = 0;
    *_tpos = c->pos;
    for (k = 0; k < c->n_cigar; ++k) {
        int op = cigar[k] & BAM_CIGAR_MASK;
        int l = cigar[k] >> BAM_CIGAR_SHIFT;
        if (op == BAM_CMATCH || op == BAM_CEQUAL || op == BAM_CDIFF) {
            if (c->pos > tpos) return y;
            if (x + l > tpos) {
                *_tpos = tpos;
                return y + (tpos - x);
            }
            x += l; y += l;
            last_y = y;
        } else if (op == BAM_CINS || op == BAM_CSOFT_CLIP) y += l;
        else if (op == BAM_CDEL || op == BAM_CREF_SKIP) {
            if (x + l > tpos) {
                *_tpos = is_left? x : x + l;
                return y;
            }
            x += l;
        }
    }
    *_tpos = x;
    return last_y;
}
// FIXME: check if the inserted sequence is consistent with the homopolymer run
// l is the relative gap length and l_run is the length of the homopolymer on the reference
static inline int est_seqQ(const bcf_callaux_t *bca, int l, int l_run)
{
    int q, qh;
    q = bca->openQ + bca->extQ * (abs(l) - 1);
    qh = l_run >= 3? (int)(bca->tandemQ * (double)abs(l) / l_run + .499) : 1000;
    return q < qh? q : qh;
}

static inline int est_indelreg(int pos, const char *ref, int l, char *ins4)
{
    int i, j, max = 0, max_i = pos, score = 0;
    l = abs(l);
    for (i = pos + 1, j = 0; ref[i]; ++i, ++j) {
        if (ins4) score += (toupper(ref[i]) != "ACGTN"[(int)ins4[j%l]])? -10 : 1;
        else score += (toupper(ref[i]) != toupper(ref[pos+1+j%l]))? -10 : 1;
        if (score < 0) break;
        if (max < score) max = score, max_i = i;
    }
    return max_i - pos;
}

// Identify spft-clip length, position in seq, and clipped seq len
static inline void get_pos(const bcf_callaux_t *bca, bam_pileup1_t *p,
                           int *sc_len_r, int *slen_r, int *epos_r, int *end) {
    bam1_t *b = p->b;
    int sc_len = 0, sc_dist = -1, at_left = 1;
    int epos = p->qpos, slen = b->core.l_qseq;
    int k;
    uint32_t *cigar = bam_get_cigar(b);
    *end = -1;
    for (k = 0; k < b->core.n_cigar; k++) {
        int op = bam_cigar_op(cigar[k]);
        if (op == BAM_CSOFT_CLIP) {
            slen -= bam_cigar_oplen(cigar[k]);
            if (at_left) {
                // left end
                sc_len += bam_cigar_oplen(cigar[k]);
                epos -= sc_len; // don't count SC in seq pos
                sc_dist = epos;
                *end = 0;
            } else {
                // right end
                int srlen = bam_cigar_oplen(cigar[k]);
                int rd = b->core.l_qseq - srlen - p->qpos;
                if (sc_dist < 0 || sc_dist > rd) {
                    // closer to right end than left
                    // FIXME: compensate for indel length too?
                    sc_dist = rd;
                    sc_len = srlen;
                    *end = 1;
                }
            }
        } else if (op != BAM_CHARD_CLIP) {
            at_left = 0;
        }
    }

    if (p->indel > 0 && slen - (epos+p->indel) < epos)
        epos += p->indel-1; // end of insertion, if near end of seq

    // slen is now length of sequence minus soft-clips and
    // epos is position of indel in seq minus left-clip.
    *epos_r = (double)epos / (slen+1) * bca->npos;

    if (sc_len) {
        // scale importance of clip by distance to closest end
        *sc_len_r = 15.0*sc_len / (sc_dist+1);
        if (*sc_len_r > 99) *sc_len_r = 99;
    } else {
        *sc_len_r = 0;
    }

    *slen_r = slen;
}

// Part of bcf_call_gap_prep.
//
// Scans the pileup to identify all the different sizes of indels
// present.
//
// Returns types and fills out n_types_r,  max_rd_len_r and ref_type_r,
//         or NULL on error.
static int *bcf_cgp_find_types(int n, int *n_plp, bam_pileup1_t **plp,
                               int pos, bcf_callaux_t *bca, const char *ref,
                               int *max_rd_len_r, int *n_types_r,
                               int *ref_type_r, int *N_r) {
    int i, j, t, s, N, m, max_rd_len, n_types;
    int n_alt = 0, n_tot = 0, indel_support_ok = 0;
    uint32_t *aux;
    int *types;

    // N is the total number of reads
    for (s = N = 0; s < n; ++s)
        N += n_plp[s];

    bca->max_support = bca->max_frac = 0;
    aux = (uint32_t*) calloc(N + 1, 4);
    if (!aux)
        return NULL;

    m = max_rd_len = 0;
    aux[m++] = MINUS_CONST; // zero indel is always a type (REF)

    // Fill out aux[] array with all the non-zero indel sizes.
    // Also tally number with indels (n_alt) and total (n_tot).
    for (s = 0; s < n; ++s) {
        int na = 0, nt = 0;
        for (i = 0; i < n_plp[s]; ++i) {
            const bam_pileup1_t *p = plp[s] + i;
            ++nt;
            if (p->indel != 0) {
                ++na;
                aux[m++] = MINUS_CONST + p->indel;
            }

            // FIXME: cache me in pileup struct.
            j = bam_cigar2qlen(p->b->core.n_cigar, bam_get_cigar(p->b));
            if (j > max_rd_len) max_rd_len = j;
        }
        double frac = (double)na/nt;
        if ( !indel_support_ok && na >= bca->min_support
             && frac >= bca->min_frac )
            indel_support_ok = 1;
        if ( na > bca->max_support && frac > 0 )
            bca->max_support = na, bca->max_frac = frac;

        n_alt += na;
        n_tot += nt;
    }

    // To prevent long stretches of N's to be mistaken for indels
    // (sometimes thousands of bases), check the number of N's in the
    // sequence and skip places where half or more reference bases are Ns.
    int nN=0;
    for (i=pos; i-pos<max_rd_len && ref[i]; i++)
        if ( ref[i]=='N' )
            nN++;
    if ( nN*2>(i-pos) ) {
        free(aux);
        return NULL;
    }

    // Sort aux[] and dedup
    ks_introsort(uint32_t, m, aux);
    for (i = 1, n_types = 1; i < m; ++i)
        if (aux[i] != aux[i-1]) ++n_types;

    // Taking totals makes it hard to call rare indels (IMF filter)
    if ( !bca->per_sample_flt )
        indel_support_ok = ( (double)n_alt / n_tot < bca->min_frac
                             || n_alt < bca->min_support )
            ? 0 : 1;
    if ( n_types == 1 || !indel_support_ok ) { // then skip
        free(aux);
        return NULL;
    }

    // Bail out if we have far too many types of indel
    if (n_types >= 64) {
        free(aux);
        // TODO revisit how/whether to control printing this warning
        if (hts_verbose >= 2)
            fprintf(stderr, "[%s] excessive INDEL alleles at position %d. "
                    "Skip the position.\n", __func__, pos + 1);
        return NULL;
    }

    // Finally fill out the types[] array detailing the size of insertion
    // or deletion.
    types = (int*)calloc(n_types, sizeof(int));
    if (!types) {
        free(aux);
        return NULL;
    }
    t = 0;
    types[t++] = aux[0] - MINUS_CONST;
    for (i = 1; i < m; ++i)
        if (aux[i] != aux[i-1])
            types[t++] = aux[i] - MINUS_CONST;
    free(aux);

    // Find reference type; types[?] == 0)
    for (t = 0; t < n_types; ++t)
        if (types[t] == 0) break;

    *ref_type_r   = t;
    *n_types_r    = n_types;
    *max_rd_len_r = max_rd_len;
    *N_r          = N;

    return types;
}

// Part of bcf_call_gap_prep.
//
// Construct per-sample consensus.
//
// Returns an array of consensus seqs,
//         or NULL on failure.
static char **bcf_cgp_ref_sample(int n, int *n_plp, bam_pileup1_t **plp,
                                 int pos, bcf_callaux_t *bca, const char *ref,
                                 int left, int right) {
    int i, k, s, L = right - left + 1, max_i, max2_i;
    char **ref_sample; // returned
    uint32_t *cns = NULL, max, max2;
    char *ref0 = NULL, *r;
    ref_sample = (char**) calloc(n, sizeof(char*));
    cns = (uint32_t*) calloc(L, 4);
    ref0 = (char*) calloc(L, 1);
    if (!ref_sample || !cns || !ref0) {
        n = 0;
        goto err;
    }

    // Convert ref ASCII to 0-15.
    for (i = 0; i < right - left; ++i)
        ref0[i] = seq_nt16_table[(int)ref[i+left]];

    // NB: one consensus per sample 'n', not per indel type.
    // FIXME: consider fixing this.  We should compute alignments vs
    // types, not vs samples?  Or types/sample combined?
    for (s = 0; s < n; ++s) {
        r = ref_sample[s] = (char*) calloc(L, 1);
        if (!r) {
            n = s-1;
            goto err;
        }

        memset(cns, 0, sizeof(int) * L);

        // collect ref and non-ref counts in cns
        for (i = 0; i < n_plp[s]; ++i) {
            bam_pileup1_t *p = plp[s] + i;
            bam1_t *b = p->b;
            uint32_t *cigar = bam_get_cigar(b);
            uint8_t *seq = bam_get_seq(b);
            int x = b->core.pos, y = 0;
            for (k = 0; k < b->core.n_cigar; ++k) {
                int op = cigar[k]&0xf;
                int j, l = cigar[k]>>4;
                if (op == BAM_CMATCH || op == BAM_CEQUAL || op == BAM_CDIFF) {
                    for (j = 0; j < l; ++j)
                        if (x + j >= left && x + j < right)
                            // Append to cns.  Note this is ref coords,
                            // so insertions aren't in cns and deletions
                            // will have lower coverage.

                            // FIXME: want true consensus (with ins) per
                            // type, so we can independently compare each
                            // seq to each consensus and see which it matches
                            // best, so we get proper GT analysis.
                            cns[x+j-left] +=
                                (bam_seqi(seq, y+j) == ref0[x+j-left])
                                ? 1        // REF
                                : (1<<16); // ALT
                    x += l; y += l;
                } else if (op == BAM_CDEL || op == BAM_CREF_SKIP) {
                    x += l;
                } else if (op == BAM_CINS || op == BAM_CSOFT_CLIP) {
                    y += l;
                }
            }
        }

        // Determine a sample specific reference.
        for (i = 0; i < right - left; ++i)
            r[i] = ref0[i];

        // Find deepest and 2nd deepest ALT region (max & max2).
        max = max2 = 0; max_i = max2_i = -1;
        for (i = 0; i < right - left; ++i) {
            if (cns[i]>>16 >= max>>16)
                max2 = max, max2_i = max_i, max = cns[i], max_i = i;
            else if (cns[i]>>16 >= max2>>16)
                max2 = cns[i], max2_i = i;
        }

        // Masks mismatches present in at least 70% of the reads with 'N'.
        // This code is nREF/(nREF+n_ALT) >= 70% for deepest region.
        // The effect is that at least 30% of bases differing to REF will
        // use "N" in consensus, so we don't penalise ALT or REF when
        // aligning against it.  (A poor man IUPAC code)
        //
        // Why is it only done in two loci at most?
        if ((double)(max&0xffff) / ((max&0xffff) + (max>>16)) >= 0.7)
            max_i = -1;
        if ((double)(max2&0xffff) / ((max2&0xffff) + (max2>>16)) >= 0.7)
            max2_i = -1;
        if (max_i >= 0) r[max_i] = 15;
        if (max2_i >= 0) r[max2_i] = 15;

        //for (i = 0; i < right - left; ++i)
        //    fputc("=ACMGRSVTWYHKDBN"[(int)r[i]], stderr);
        //fputc('\n', stderr);
    }

    free(ref0);
    free(cns);

    return ref_sample;

 err:
    free(ref0);
    free(cns);
    if (ref_sample) {
        for (s = 0; s < n; s++)
            free(ref_sample[s]);
        free(ref_sample);
    }

    return NULL;
}

// The length of the homopolymer run around the current position
static int bcf_cgp_l_run(const char *ref, int pos) {
    int i, l_run;

    int c = seq_nt16_table[(int)ref[pos + 1]];
    if (c == 15) {
        l_run = 1;
    } else {
        for (i = pos + 2; ref[i]; ++i)
            if (seq_nt16_table[(int)ref[i]] != c) break;
        l_run = i;
        for (i = pos; i >= 0; --i)
            if (seq_nt16_table[(int)ref[i]] != c) break;
        l_run -= i + 1;
    }

    return l_run;
}


// Compute the consensus for this sample 's', minus indels which
// get added later.
static char *bcf_cgp_calc_cons(int n, int *n_plp, bam_pileup1_t **plp,
                               int pos, int *types, int n_types,
                               int max_ins, int s) {
    int i, j, t, k;
    int *inscns_aux = (int*)calloc(5 * n_types * max_ins, sizeof(int));
    if (!inscns_aux)
        return NULL;

    // Count the number of occurrences of each base at each position for
    // each type of insertion.
    for (t = 0; t < n_types; ++t) {
        if (types[t] > 0) {
            for (s = 0; s < n; ++s) {
                for (i = 0; i < n_plp[s]; ++i) {
                    bam_pileup1_t *p = plp[s] + i;
                    if (p->indel == types[t]) {
                        uint8_t *seq = bam_get_seq(p->b);
                        for (k = 1; k <= p->indel; ++k) {
                            int c = seq_nt16_int[bam_seqi(seq, p->qpos + k)];
                            assert(c<5);
                            ++inscns_aux[(t*max_ins+(k-1))*5 + c];
                        }
                    }
                }
            }
        }
    }

    // Use the majority rule to construct the consensus
    char *inscns = (char *)calloc(n_types * max_ins, 1);
    for (t = 0; t < n_types; ++t) {
        for (j = 0; j < types[t]; ++j) {
            int max = 0, max_k = -1, *ia = &inscns_aux[(t*max_ins+j)*5];
            for (k = 0; k < 5; ++k)
                if (ia[k] > max)
                    max = ia[k], max_k = k;
            inscns[t*max_ins + j] = max ? max_k : 4;
            if (max_k == 4) {
                // discard insertions which contain N's
                types[t] = 0;
                break;
            }
        }
    }
    free(inscns_aux);

    return inscns;
}

// Part of bcf_call_gap_prep.
//
// Realign using BAQ to get an alignment score of a single read vs
// a haplotype consensus.
//
// Fills out score1 and score2.
// Returns 0 on success,
//        <0 on error
static int bcf_cgp_align_score(bam_pileup1_t *p, bcf_callaux_t *bca,
                               int type, uint8_t *ref2, uint8_t *query,
                               int tbeg_, int tbeg, int tend,
                               int left, int right,
                               int qbeg, int qend,
                               int qpos, int max_deletion,
                               int *score1, int *score2) {
    probaln_par_t apf1 = { 1e-4, 1e-2, 10 }, apf2 = { 1e-6, 1e-3, 10 };
    double nqual_over_60 = bca->nqual / 60.0;
    type = abs(type);
    apf1.bw = apf2.bw = type + 3;
    int k, l, sc;
    const uint8_t *qual = bam_get_qual(p->b), *bq;
    uint8_t *qq;
    qq = (uint8_t*) calloc(qend - qbeg, 1);
    if (!qq)
        return -1;
    bq = (uint8_t*)bam_aux_get(p->b, "ZQ");
    if (bq) ++bq; // skip type
    for (l = qbeg; l < qend; ++l) {
        qq[l - qbeg] = bq? qual[l] + (bq[l] - 64) : qual[l];
        if (qq[l - qbeg] > 30) qq[l - qbeg] = 30;
        if (qq[l - qbeg] < 7) qq[l - qbeg] = 7;
    }

    sc = probaln_glocal(ref2 + tbeg - left, tend - tbeg + type,
                        query, qend - qbeg, qq, &apf1, 0, 0);
    l = (int)(100. * sc / (qend - qbeg) + .499); // used for adjusting indelQ below
    l *= bca->indel_bias;

    if (l > 255) l = 255;
    *score1 = *score2 = sc<<8 | l;
    if (sc > 5) {
        sc = probaln_glocal(ref2 + tbeg - left, tend - tbeg + type,
                            query, qend - qbeg, qq, &apf2, 0, 0);
        l = (int)(100. * sc / (qend - qbeg) + .499);
        l *= bca->indel_bias;
        if (l > 255) l = 255;
        *score2 = sc<<8 | l;
    }

    rep_ele *reps, *elt, *tmp;
    uint8_t *seg = ref2 + tbeg - left;
    int seg_len = tend - tbeg + type;

    // FIXME: need to make this work on IUPAC
    reps = find_STR((char *)seg, seg_len, 0);
    int iscore = 0;

    // Identify STRs in ref covering the indel up to
    // (or close to) the end of the sequence.
    // Those having an indel and right at the sequence
    // end do not confirm the total length of indel
    // size.  Specifically a *lack* of indel at the
    // end, where we know indels occur in other
    // sequences, is a possible reference bias.
    //
    // This is emphasised further if the sequence ends with
    // soft clipping.
    DL_FOREACH_SAFE(reps, elt, tmp) {
        if (elt->start <= qpos && elt->end >= qpos) {
#define END_SLOP 5
            // FIXME: wrong coord used here!
            if (elt->start - END_SLOP <= tbeg_ ||
                elt->end   + END_SLOP >= tend) {
                // STR copy number
                iscore += (elt->end-elt->start) / elt->rep_len;
            }
        }

        DL_DELETE(reps, elt);
        free(elt);
    }
    if (iscore > 99) iscore = 99;
    if (p->indel)
        bca->ialt_str[iscore]++;
    else
        bca->iref_str[iscore]++;

    sc = *score1>>8;
    l  =  *score1&0xff;
    l = l*.8 + iscore*2;
    if (l>255) l=255;
    *score1 = sc<<8 | l;

    sc = *score2>>8;
    l  = *score2&0xff;
    l  = l*.8 + iscore*2;
    if (l>255) l=255;
    *score2 = sc<<8 | l;


    // FIXME: don't need to do on all types, just 1?

    // Find minimum adjusted quality within +/- HALO bases
    // of this indel.  Use this for BQBZ metric.
#define HALO 10
    int min_bq = INT_MAX;
    int qbeg2 = p->qpos - HALO;
    int qend2 = p->qpos+1 + HALO + max_deletion;

    if (p->indel>0) qend2 += p->indel;
    if (qbeg2 < qbeg)
        qbeg2 = qbeg;
    if (qend2 > qend)
        qend2 = qend;

    for (k = qbeg2; k < qend2; k++)
        if (min_bq > qq[k-qbeg])
            min_bq = qq[k-qbeg];
    if (min_bq > 59) min_bq = 59;
    min_bq *= nqual_over_60;
    if (p->indel)
        bca->ialt_bq[min_bq]++;
    else
        bca->iref_bq[min_bq]++;

    free(qq);

    return 0;
}

// Part of bcf_call_gap_prep.
//
// Returns n_alt on success
//         -1 on failure
static int bcf_cgp_compute_indelQ(int n, int *n_plp, bam_pileup1_t **plp,
                                  bcf_callaux_t *bca, char *inscns,
                                  int l_run, int max_ins,
                                  int ref_type, int *types, int n_types,
                                  int *score1, int *score2) {
    // FIXME: n_types has a maximum; no need to alloc - use a #define?
    int sc_a[16], sumq_a[16], s, i, j, t, K, n_alt;
    int tmp, *sc = sc_a, *sumq = sumq_a;
    if (n_types > 16) {
        sc   = (int *)malloc(n_types * sizeof(int));
        sumq = (int *)malloc(n_types * sizeof(int));
    }
    memset(sumq, 0, n_types * sizeof(int));
    for (s = K = 0; s < n; ++s) {
        for (i = 0; i < n_plp[s]; ++i, ++K) {
            bam_pileup1_t *p = plp[s] + i;
            int *sct = &score1[K*n_types], indelQ1, indelQ2, seqQ, indelQ;
            for (t = 0; t < n_types; ++t) sc[t] = sct[t]<<6 | t;
            for (t = 1; t < n_types; ++t) // insertion sort
                for (j = t; j > 0 && sc[j] < sc[j-1]; --j)
                    tmp = sc[j], sc[j] = sc[j-1], sc[j-1] = tmp;
            /* errmod_cal() assumes that if the call is wrong, the
             * likelihoods of other events are equal. This is about
             * right for substitutions, but is not desired for
             * indels. To reuse errmod_cal(), I have to make
             * compromise for multi-allelic indels.
             */
            if ((sc[0]&0x3f) == ref_type) {
                indelQ1 = (sc[1]>>14) - (sc[0]>>14);
                seqQ = est_seqQ(bca, types[sc[1]&0x3f], l_run);
            } else {
                for (t = 0; t < n_types; ++t) // look for the reference type
                    if ((sc[t]&0x3f) == ref_type) break;
                indelQ1 = (sc[t]>>14) - (sc[0]>>14);
                seqQ = est_seqQ(bca, types[sc[0]&0x3f], l_run);
            }
            tmp = sc[0]>>6 & 0xff;
            indelQ1 = tmp > 111? 0 : (int)((1. - tmp/111.) * indelQ1 + .499); // reduce indelQ
            sct = &score2[K*n_types];
            for (t = 0; t < n_types; ++t) sc[t] = sct[t]<<6 | t;
            for (t = 1; t < n_types; ++t) // insertion sort
                for (j = t; j > 0 && sc[j] < sc[j-1]; --j)
                    tmp = sc[j], sc[j] = sc[j-1], sc[j-1] = tmp;
            if ((sc[0]&0x3f) == ref_type) {
                indelQ2 = (sc[1]>>14) - (sc[0]>>14);
            } else {
                for (t = 0; t < n_types; ++t) // look for the reference type
                    if ((sc[t]&0x3f) == ref_type) break;
                indelQ2 = (sc[t]>>14) - (sc[0]>>14);
            }
            tmp = sc[0]>>6 & 0xff;
            indelQ2 = tmp > 111? 0 : (int)((1. - tmp/111.) * indelQ2 + .499);
            // pick the smaller between indelQ1 and indelQ2
            indelQ = indelQ1 < indelQ2? indelQ1 : indelQ2;

            // Doesn't really help accuracy, but permits -h to take
            // affect still.
            if (indelQ > seqQ) indelQ = seqQ;

            if (indelQ > 255) indelQ = 255;
            if (seqQ > 255) seqQ = 255;
            p->aux = (sc[0]&0x3f)<<16 | seqQ<<8 | indelQ; // use 22 bits in total
            sumq[sc[0]&0x3f] += indelQ < seqQ? indelQ : seqQ;
            //              fprintf(stderr, "pos=%d read=%d:%d name=%s call=%d indelQ=%d seqQ=%d\n", pos, s, i, bam1_qname(p->b), types[sc[0]&0x3f], indelQ, seqQ);
        }
    }
    // determine bca->indel_types[] and bca->inscns
    bca->maxins = max_ins;
    bca->inscns = (char*) realloc(bca->inscns, bca->maxins * 4);
    if (bca->maxins && !bca->inscns)
        return -1;
    for (t = 0; t < n_types; ++t)
        sumq[t] = sumq[t]<<6 | t;
    for (t = 1; t < n_types; ++t) // insertion sort
        for (j = t; j > 0 && sumq[j] > sumq[j-1]; --j)
            tmp = sumq[j], sumq[j] = sumq[j-1], sumq[j-1] = tmp;
    for (t = 0; t < n_types; ++t) // look for the reference type
        if ((sumq[t]&0x3f) == ref_type) break;
    if (t) { // then move the reference type to the first
        tmp = sumq[t];
        for (; t > 0; --t) sumq[t] = sumq[t-1];
        sumq[0] = tmp;
    }
    for (t = 0; t < 4; ++t) bca->indel_types[t] = B2B_INDEL_NULL;
    for (t = 0; t < 4 && t < n_types; ++t) {
        bca->indel_types[t] = types[sumq[t]&0x3f];
        memcpy(&bca->inscns[t * bca->maxins], &inscns[(sumq[t]&0x3f) * max_ins], bca->maxins);
    }
    // update p->aux
    for (s = n_alt = 0; s < n; ++s) {
        for (i = 0; i < n_plp[s]; ++i) {
            bam_pileup1_t *p = plp[s] + i;
            int x = types[p->aux>>16&0x3f];
            for (j = 0; j < 4; ++j)
                if (x == bca->indel_types[j]) break;
            p->aux = j<<16 | (j == 4? 0 : (p->aux&0xffff));
            if ((p->aux>>16&0x3f) > 0) ++n_alt;
            //fprintf(stderr, "X pos=%d read=%d:%d name=%s call=%d type=%d seqQ=%d indelQ=%d\n", pos, s, i, bam_get_qname(p->b), (p->aux>>16)&0x3f, bca->indel_types[(p->aux>>16)&0x3f], (p->aux>>8)&0xff, p->aux&0xff);
        }
    }

    if (sc   != sc_a)   free(sc);
    if (sumq != sumq_a) free(sumq);

    return n_alt;
}

/*
FIXME: with high number of samples, do we handle IMF correctly?  Is it
fraction of indels across entire data set, or just fraction for this
specific sample? Needs to check bca->per_sample_flt (--per-sample-mF) opt.
 */

/*
    notes:
        - n .. number of samples
        - the routine sets bam_pileup1_t.aux of each read as follows:
            - 6: unused
            - 6: the call; index to bcf_callaux_t.indel_types   .. (aux>>16)&0x3f
            - 8: estimated sequence quality                     .. (aux>>8)&0xff
            - 8: indel quality                                  .. aux&0xff
 */
// 1. Find out how many types of indels are present.
// 2. Construct per-sample consensus
// 3. Check length of homopolymer run around the current positions (l_run)
//    FIXME: length of STR rather than pure homopolymer?
// 4. Construct consensus sequence (how diff to 2?)
// 5. Compute likelihood given each type of indel for each read
//    This does the probaln_glocal step, setting score1[] and score2[]
// 6. Compute indelQ
int bcf_call_gap_prep(int n, int *n_plp, bam_pileup1_t **plp, int pos, bcf_callaux_t *bca, const char *ref)
{
    if (ref == 0 || bca == 0) return -1;

    int i, s, j, k, t, n_types, *types, max_rd_len, left, right, max_ins;
    int *score1, *score2, max_ref2;
    int N, K, l_run, ref_type, n_alt;
    char *inscns = 0, *ref2, *query, **ref_sample;

    // determine if there is a gap
    for (s = N = 0; s < n; ++s) {
        for (i = 0; i < n_plp[s]; ++i)
            if (plp[s][i].indel != 0) break;
        if (i < n_plp[s]) break;
    }
    if (s == n)
        // there is no indel at this position.
        return -1;

    // find out how many types of indels are present
    types = bcf_cgp_find_types(n, n_plp, plp, pos, bca, ref,
                               &max_rd_len, &n_types, &ref_type, &N);
    if (!types)
        return -1;


    // calculate left and right boundary
    left = pos > INDEL_WINDOW_SIZE? pos - INDEL_WINDOW_SIZE : 0;
    right = pos + INDEL_WINDOW_SIZE;
    if (types[0] < 0) right -= types[0];

    // in case the alignments stand out the reference
    for (i = pos; i < right; ++i)
        if (ref[i] == 0) break;
    right = i;


    /* The following call fixes a long-existing flaw in the INDEL
     * calling model: the interference of nearby SNPs. However, it also
     * reduces the power because sometimes, substitutions caused by
     * indels are not distinguishable from true mutations. Multiple
     * sequence realignment helps to increase the power.
     *
     * Masks mismatches present in at least 70% of the reads with 'N'.
     */
    ref_sample = bcf_cgp_ref_sample(n, n_plp, plp, pos, bca, ref, left, right);

    // The length of the homopolymer run around the current position
    l_run = bcf_cgp_l_run(ref, pos);

    // construct the consensus sequence (minus indels, which are added later)
    max_ins = types[n_types - 1];   // max_ins is at least 0
    if (max_ins > 0) {
        inscns = bcf_cgp_calc_cons(n, n_plp, plp, pos,
                                   types, n_types, max_ins, s);
        if (!inscns)
            return -1;
    }

    // compute the likelihood given each type of indel for each read
    max_ref2 = right - left + 2 + 2 * (max_ins > -types[0]? max_ins : -types[0]);
    ref2  = (char*) calloc(max_ref2, 1);
    query = (char*) calloc(right - left + max_rd_len + max_ins + 2, 1);
    score1 = (int*) calloc(N * n_types, sizeof(int));
    score2 = (int*) calloc(N * n_types, sizeof(int));
    bca->indelreg = 0;
    double nqual_over_60 = bca->nqual / 60.0;
    for (t = 0; t < n_types; ++t) {
        int l, ir;

        // compute indelreg
        if (types[t] == 0)
            ir = 0;
        else if (types[t] > 0)
            ir = est_indelreg(pos, ref, types[t], &inscns[t*max_ins]);
        else
            ir = est_indelreg(pos, ref, -types[t], 0);

        if (ir > bca->indelreg)
            bca->indelreg = ir;

        // Identify max deletion length
        int max_deletion = 0;
        for (s = 0; s < n; ++s) {
            for (i = 0; i < n_plp[s]; ++i, ++K) {
                bam_pileup1_t *p = plp[s] + i;
                if (max_deletion < -p->indel)
                    max_deletion = -p->indel;
            }
        }

        // Realignment score, computed via BAQ
        for (s = K = 0; s < n; ++s) {
            // Construct ref2 from ref_sample, inscns and indels.
            // This is now the true sample consensus (possibly prepended
            // and appended with reference if sample data doesn't span
            // the full length).
            for (k = 0, j = left; j <= pos; ++j)
                ref2[k++] = seq_nt16_int[(int)ref_sample[s][j-left]];

            if (types[t] <= 0)
                j += -types[t];
            else
                for (l = 0; l < types[t]; ++l)
                    ref2[k++] = inscns[t*max_ins + l];

            for (; j < right && ref[j]; ++j)
                ref2[k++] = seq_nt16_int[(int)ref_sample[s][j-left]];
            for (; k < max_ref2; ++k)
                ref2[k] = 4;

            if (right > j)
                right = j;

            // align each read to ref2
            for (i = 0; i < n_plp[s]; ++i, ++K) {
                bam_pileup1_t *p = plp[s] + i;

                // Some basic ref vs alt stats.
                int imq = p->b->core.qual > 59 ? 59 : p->b->core.qual;
                imq *= nqual_over_60;

                int sc_len, slen, epos, sc_end;

                // FIXME: don't need to do on all types, just 1?
                // Need somewhere to cache the data (eg pileup struct)
                get_pos(bca, p, &sc_len, &slen, &epos, &sc_end);

                // Gather stats for INFO field to aid filtering.
                // mq and sc_len not very helpful for filtering, but could
                // help in assigning a better QUAL value.
                //
                // Pos is slightly useful.
                // Base qual can be useful, but need qual prior to BAQ?
                // May need to cache orig quals in aux tag so we can fetch
                // them even after mpileup step.

                // FIXME: don't need to do on all types, just 1?
                assert(imq >= 0 && imq < bca->nqual);
                assert(epos >= 0 && epos < bca->npos);
                assert(sc_len >= 0 && sc_len < 100);
                if (p->indel) {
                    bca->ialt_mq[imq]++;
                    bca->ialt_scl[sc_len]++;
                    bca->ialt_pos[epos]++;
                } else {
                    bca->iref_mq[imq]++;
                    bca->iref_scl[sc_len]++;
                    bca->iref_pos[epos]++;
                }

                int qbeg, qend, tbeg, tend, kk;
                uint8_t *seq = bam_get_seq(p->b);
                uint32_t *cigar = bam_get_cigar(p->b);
                if (p->b->core.flag & BAM_FUNMAP) continue;
                // FIXME: the following loop should be better moved outside;
                // nonetheless, realignment should be much slower anyway.
                for (kk = 0; kk < p->b->core.n_cigar; ++kk)
                    if ((cigar[kk]&BAM_CIGAR_MASK) == BAM_CREF_SKIP)
                        break;
                if (kk < p->b->core.n_cigar)
                    continue;

                // FIXME: the following skips soft clips, but using them
                // may be more sensitive.

                // determine the start and end of sequences for alignment
                // FIXME: loops over CIGAR multiple times
                qbeg = tpos2qpos(&p->b->core, bam_get_cigar(p->b), left,
                                 0, &tbeg);
                int qpos = tpos2qpos(&p->b->core, bam_get_cigar(p->b), pos,
                                     0, &tend) - qbeg;
                qend = tpos2qpos(&p->b->core, bam_get_cigar(p->b), right,
                                 1, &tend);
                int tbeg_ = tbeg;
                if (types[t] < 0) {
                    int l = -types[t];
                    tbeg = tbeg - l > left?  tbeg - l : left;
                }

                // write the query sequence
                for (l = qbeg; l < qend; ++l)
                    query[l - qbeg] = seq_nt16_int[bam_seqi(seq, l)];

                // do realignment; this is the bottleneck
                if (bcf_cgp_align_score(p, bca, types[t],
                                        (uint8_t *)ref2, (uint8_t *)query,
                                        tbeg_, tbeg, tend, left, right,
                                        qbeg, qend, qpos, max_deletion,
                                        &score1[K*n_types + t],
                                        &score2[K*n_types + t]) < 0)
                    return -1;
#if 0
                for (l = 0; l < tend - tbeg + abs(types[t]); ++l)
                    fputc("ACGTN"[(int)ref2[tbeg-left+l]], stderr);
                fputc('\n', stderr);
                for (l = 0; l < qend - qbeg; ++l)
                    fputc("ACGTN"[(int)query[l]], stderr);
                fputc('\n', stderr);
                fprintf(stderr, "pos=%d type=%d read=%d:%d name=%s "
                        "qbeg=%d tbeg=%d score=%d\n",
                        pos, types[t], s, i, bam_get_qname(p->b),
                        qbeg, tbeg, sc);
#endif
            }
        }
    }

    // compute indelQ
    n_alt = bcf_cgp_compute_indelQ(n, n_plp, plp, bca, inscns, l_run, max_ins,
                                   ref_type, types, n_types, score1, score2);

    // free
    free(ref2);
    free(query);
    free(score1);
    free(score2);

    for (i = 0; i < n; ++i)
        free(ref_sample[i]);

    free(ref_sample);
    free(types); free(inscns);

    return n_alt > 0? 0 : -1;
}
