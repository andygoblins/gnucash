/********************************************************************\
 * cap-gains.c -- Automatically Compute Capital Gains/Losses        *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
\********************************************************************/

/** @file cap-gains.c
 *  @breif Utilities to Automatically Compute Capital Gains/Losses.
 *  @author Created by Linas Vepstas August 2003
 *  @author Copyright (c) 2003 Linas Vepstas <linas@linas.org>
 *
 *  This file implements the various routines to automatically
 *  compute and handle Cap Gains/Losses resulting from trading 
 *  activities.  Some of these routines might have broader 
 *  applicability, for handling depreciation *  & etc. 
 *
 *  This code is under development, and is 'alpha': many important
 *  routines are missing, many existing routines are not called 
 *  from inside the engine as needed, and routines may be buggy.
 *
 *  This code does not currently handle tax distinctions, e.g
 *  the different tax treatment that short-term and long-term 
 *  cap gains have. 

ToDo List:
 o Need to use a 'gains dirty' flag: A 'dirty' flag on the source 
   split indicates that the gains transaction needs to be recomputed.
   Another flag, the gains transaction flag, marks the split as
   being a gains split, and that the source transaction should be 
   checked for dirtiness before returning the date, the amount, the
   value, etc.  Finally, these flags make amount and value read-only
   for the gains splits. (the memo is user-modifieable).

 o If the amount in a split is changed, then the lot has to be recomputed.
   This has a potential trickle-through effect on all later lots. 
   Ideally, later lots are dissolved, and recomputed.  However, some 
   lots may have been user-hand-built. These should be left alone.

 o XXX if the split has been split, and the lots need to be recomputed,
   then the peers need to be reunified first!   And that implies that
   gain transactions need to be 'reunified' too.

 o XXX Need to create a data-integrity scrubber, tht makes sure that
   the various flags, and pointers & etc. match. See sections marked
   with XXX below for things that might go wrong.
 */

#include "config.h"

#include <glib.h>

#include "Account.h"
#include "AccountP.h"
#include "Group.h"
#include "GroupP.h"
#include "Transaction.h"
#include "TransactionP.h"
#include "cap-gains.h"
#include "gnc-engine.h"
#include "gnc-engine-util.h"
#include "gnc-lot.h"
#include "gnc-lot-p.h"
#include "gnc-trace.h"
#include "kvp-util-p.h"
#include "messages.h"
#include "qofid-p.h"

static short module = MOD_LOT;


/* ============================================================== */

gboolean 
xaccAccountHasTrades (Account *acc)
{
   gnc_commodity *acc_comm;
   SplitList *node;

   if (!acc) return FALSE;

   acc_comm = acc->commodity;

   for (node=acc->splits; node; node=node->next)
   {
      Split *s = node->data;
      Transaction *t = s->parent;
      if (acc_comm != t->common_currency) return TRUE;
   }

   return FALSE;
}

/* ============================================================== */

struct early_lot_s
{
   GNCLot *lot;
   Timespec ts;
   int (*numeric_pred)(gnc_numeric);
};

static gpointer earliest_helper (GNCLot *lot,  gpointer user_data)
{
   struct early_lot_s *els = user_data;
   Split *s;
   Transaction *trans;
   gnc_numeric bal;

   if (gnc_lot_is_closed (lot)) return NULL;

   /* We want a lot whose balance is of the correct sign */
   bal = gnc_lot_get_balance (lot);
   if (0 == (els->numeric_pred) (bal)) return NULL;
   
   s = gnc_lot_get_earliest_split (lot);
   trans = s->parent;
   if ((els->ts.tv_sec > trans->date_posted.tv_sec)  ||
       ((els->ts.tv_sec == trans->date_posted.tv_sec) &&
        (els->ts.tv_nsec > trans->date_posted.tv_nsec)))
   {
      els->ts = trans->date_posted;
      els->lot = lot;
   }
   
   return NULL;
}

GNCLot *
xaccAccountFindEarliestOpenLot (Account *acc, gnc_numeric sign)
{
   struct early_lot_s es;

   es.lot = NULL;
   es.ts.tv_sec = 10000000LL * ((long long) LONG_MAX);
   es.ts.tv_nsec = 0;

   if (gnc_numeric_positive_p(sign)) es.numeric_pred = gnc_numeric_negative_p;
   else es.numeric_pred = gnc_numeric_positive_p;
      
   xaccAccountForEachLot (acc, earliest_helper, &es);
   return es.lot;
}

/* ============================================================== */
/* Similar to GetOrMakeAccount, but different in important ways */

static Account *
GetOrMakeLotOrphanAccount (AccountGroup *root, gnc_commodity * currency)
{
  char * accname;
  Account * acc;

  g_return_val_if_fail (root, NULL);

  /* build the account name */
  if (!currency)
  {
    PERR ("No currency specified!");
    return NULL;
  }

  accname = g_strconcat (_("Orphaned Gains"), "-",
                         gnc_commodity_get_mnemonic (currency), NULL);

  /* See if we've got one of these going already ... */
  acc = xaccGetAccountFromName (root, accname);

  if (acc == NULL)
  {
    /* Guess not. We'll have to build one. */
    acc = xaccMallocAccount (root->book);
    xaccAccountBeginEdit (acc);
    xaccAccountSetName (acc, accname);
    xaccAccountSetCommodity (acc, currency);
    xaccAccountSetType (acc, INCOME);
    xaccAccountSetDescription (acc, _("Realized Gain/Loss"));
    xaccAccountSetNotes (acc, 
         _("Realized Gains or Losses from\n"
           "Commodity or Trading Accounts\n"
           "that haven't been recorded elsewhere.\n"));

    /* Hang the account off the root. */
    xaccGroupInsertAccount (root, acc);
    xaccAccountCommitEdit (acc);
  }

  g_free (accname);

  return acc;
}

/* ============================================================== */

void
xaccAccountSetDefaultGainAccount (Account *acc, Account *gain_acct)
{
  KvpFrame *cwd;
  KvpValue *vvv;
  const char * cur_name;

  if (!acc || !gain_acct) return;

  cwd = xaccAccountGetSlots (acc);
  cwd = kvp_frame_get_frame_slash (cwd, "/lot-mgmt/gains-act/");

  /* Accounts are indexed by thier unique currency name */
  cur_name = gnc_commodity_get_unique_name (acc->commodity);

  xaccAccountBeginEdit (acc);
  vvv = kvp_value_new_guid (xaccAccountGetGUID (gain_acct));
  kvp_frame_set_slot_nc (cwd, cur_name, vvv);
  xaccAccountSetSlots_nc (acc, acc->kvp_data);
  xaccAccountCommitEdit (acc);
}

/* ============================================================== */

Account *
xaccAccountGetDefaultGainAccount (Account *acc, gnc_commodity * currency)
{
  Account *gain_acct = NULL;
  KvpFrame *cwd;
  KvpValue *vvv;
  GUID * gain_acct_guid;
  const char * cur_name;

  if (!acc || !currency) return NULL;

  cwd = xaccAccountGetSlots (acc);
  cwd = kvp_frame_get_frame_slash (cwd, "/lot-mgmt/gains-act/");

  /* Accounts are indexed by thier unique currency name */
  cur_name = gnc_commodity_get_unique_name (currency);
  vvv = kvp_frame_get_slot (cwd, cur_name);
  gain_acct_guid = kvp_value_get_guid (vvv);

  gain_acct = xaccAccountLookup (gain_acct_guid, acc->book);
  return gain_acct;
}

/* ============================================================== */
/* Functionally identical to the following:
 *   if (!xaccAccountGetDefaultGainAccount()) xaccAccountSetDefaultGainAccount ();
 * except that it saves a few cycles.
 */

static Account *
GetOrMakeGainAcct (Account *acc, gnc_commodity * currency)
{
  Account *gain_acct = NULL;
  KvpFrame *cwd;
  KvpValue *vvv;
  GUID * gain_acct_guid;
  const char * cur_name;

  cwd = xaccAccountGetSlots (acc);
  cwd = kvp_frame_get_frame_slash (cwd, "/lot-mgmt/gains-act/");

  /* Accounts are indexed by thier unique currency name */
  cur_name = gnc_commodity_get_unique_name (currency);
  vvv = kvp_frame_get_slot (cwd, cur_name);
  gain_acct_guid = kvp_value_get_guid (vvv);

  gain_acct = xaccAccountLookup (gain_acct_guid, acc->book);

  /* If there is no default place to put gains/losses 
   * for this account, then create such a place */
  if (NULL == gain_acct)
  {
      AccountGroup *root;

      xaccAccountBeginEdit (acc);
      root = xaccAccountGetRoot(acc);
      gain_acct = GetOrMakeLotOrphanAccount (root, currency);

      vvv = kvp_value_new_guid (xaccAccountGetGUID (gain_acct));
      kvp_frame_set_slot_nc (cwd, cur_name, vvv);
      xaccAccountSetSlots_nc (acc, acc->kvp_data);
      xaccAccountCommitEdit (acc);

  }
  return gain_acct;
}

/* ============================================================== */
/* Accounting-policy callback.  Given an account and an amount, 
 * this routine should return a lot.  By implementing this as 
 * a callback, we can 'easily' add other accounting policies.
 * Currently, we only implement the FIFO policy.
 */
typedef GNCLot * (*AccountingPolicy) (Account *, 
                                      Split *, 
                                      gpointer user_data);
static gboolean
xaccSplitAssignToLot (Split *split, 
                      AccountingPolicy policy, gpointer user_data)
{
   Account *acc;
   gboolean splits_added = FALSE;
   GNCLot *lot;

   if (!split) return FALSE;

   ENTER ("split=%p", split);

   /* If this split already belongs to a lot, we are done. */
   if (split->lot) return FALSE;
   acc = split->acc;
   xaccAccountBeginEdit (acc);

   /* If we are here, this split does not belong to any lot.
    * Lets put it in the earliest one we can find.  This 
    * block is written in the form of a while loop, since we
    * may have to bust a split across several lots.
    */
  while (split)
  {
     PINFO ("have split amount=%s", gnc_numeric_to_string (split->amount));
     split->gains |= GAINS_STATUS_VDIRTY;
     lot = policy (acc, split, user_data);
     if (lot)
     {
        /* If the amount is smaller than open balance ... */
        gnc_numeric baln = gnc_lot_get_balance (lot);
        int cmp = gnc_numeric_compare (gnc_numeric_abs(split->amount),
                                       gnc_numeric_abs(baln));

        PINFO ("found open lot with baln=%s", gnc_numeric_to_string (baln));
        /* cmp == +1 if amt > baln */
        if (0 < cmp) 
        {
           time_t now = time(0);
           Split * new_split;
           gnc_numeric amt_a, amt_b, amt_tot;
           gnc_numeric val_a, val_b, val_tot;
           gnc_numeric tmp;
           Transaction *trans;
           Timespec ts;

           trans = split->parent;
           xaccTransBeginEdit (trans);

           amt_tot = split->amount;
           amt_a = gnc_numeric_neg (baln);
           amt_b = gnc_numeric_sub_fixed (amt_tot, amt_a);

           PINFO ("++++++++++++++ splitting split into amt = %s + %s",
                   gnc_numeric_to_string(amt_a),
                   gnc_numeric_to_string(amt_b) );

           /* Compute the value so that it holds in the same proportion:
            * i.e. so that (amt_a / amt_tot) = (val_a / val_tot)
            */
           val_tot = split->value;
           val_a = gnc_numeric_mul (amt_a, val_tot, 
                             GNC_DENOM_AUTO, GNC_DENOM_REDUCE);
           tmp = gnc_numeric_div (val_a, amt_tot, 
                             gnc_numeric_denom(val_tot), GNC_DENOM_EXACT);

           val_a = tmp;
           val_b = gnc_numeric_sub_fixed (val_tot, val_a);

           PINFO ("split value is = %s = %s + %s",
                   gnc_numeric_to_string(val_tot),
                   gnc_numeric_to_string(val_a),
                   gnc_numeric_to_string(val_b) );
     
           xaccSplitSetAmount (split, amt_a);
           xaccSplitSetValue (split, val_a);

           /* Adding this split will have the effect of closing this lot,
            * because the new balance should be precisely zero. */
           gnc_lot_add_split (lot, split);

           /* Put the remainder of the balance into a new split, 
            * which is in other respects just a clone of this one. */
           new_split = xaccMallocSplit (acc->book);

           /* Copy most of the split attributes */
           xaccSplitSetMemo (new_split, xaccSplitGetMemo (split));
           xaccSplitSetAction (new_split, xaccSplitGetAction (split));
           xaccSplitSetReconcile (new_split, xaccSplitGetReconcile (split));
           ts = xaccSplitRetDateReconciledTS (split);
           xaccSplitSetDateReconciledTS (new_split, &ts);

           /* We do not copy the KVP tree, as it seems like a dangerous
            * thing to do.  If the user wants to access stuff in the 'old'
            * kvp tree from the 'new' split, they shoudl follow the 
            * 'split-lot' pointers.  Yes, this is complicated, but what
            * else can one do ??
            */
           /* Add kvp markup to indicate that these two splits used 
            * to be one before being 'split' 
            */
           gnc_kvp_array (split->kvp_data, "/lot-split", now, 
                           "peer_guid", xaccSplitGetGUID (new_split), 
                           NULL);

           gnc_kvp_array (new_split->kvp_data, "/lot-split", now, 
                           "peer_guid", xaccSplitGetGUID (split), 
                           NULL);

           xaccSplitSetAmount (new_split, amt_b);
           xaccSplitSetValue (new_split, val_b);
           
           xaccAccountInsertSplit (acc, new_split);
           xaccTransAppendSplit (trans, new_split);
           xaccTransCommitEdit (trans);
           split = new_split;

           splits_added = TRUE;
        }
        else
        {
           gnc_lot_add_split (lot, split);
           split = NULL;
           PINFO ("added split to lot, new lot baln=%s", 
                gnc_numeric_to_string (gnc_lot_get_balance(lot)));
        }
     }
     else
     {
        gint64 id;
        char buff[200];

        /* No lot was found.  Start a new lot */
        PINFO ("start new lot");
        lot = gnc_lot_new (acc->book);
        gnc_lot_add_split (lot, split);
        split = NULL;

        /* Provide a reasonable title for the new lot */
        id = kvp_frame_get_gint64 (xaccAccountGetSlots (acc), "/lot-mgmt/next-id");
        snprintf (buff, 200, _("Lot %lld"), id);
        kvp_frame_set_str (gnc_lot_get_slots (lot), "/title", buff);
        id ++;
        kvp_frame_set_gint64 (xaccAccountGetSlots (acc), "/lot-mgmt/next-id", id);
     }
   }
   xaccAccountCommitEdit (acc);

   LEAVE ("added=%d", splits_added);
   return splits_added;
}

static GNCLot * 
FIFOPolicy (Account *acc, Split *split, gpointer user_data)
{
   return xaccAccountFindEarliestOpenLot (acc, split->amount);
}

gboolean
xaccSplitFIFOAssignToLot (Split *split)
{
   return xaccSplitAssignToLot (split, FIFOPolicy, NULL);
}

/* ============================================================== */

Split *
xaccSplitGetCapGainsSplit (Split *split)
{
   KvpValue *val;
   GUID *gains_guid;
   Split *gains_split;
   
   if (!split) return NULL;

   val = kvp_frame_get_slot (split->kvp_data, "gains-split");
   if (!val) return NULL;
   gains_guid = kvp_value_get_guid (val);
   if (!gains_guid) return NULL;

   gains_split = qof_entity_lookup (qof_book_get_entity_table(split->book),
                   gains_guid, GNC_ID_SPLIT);
   return gains_split;
}

/* ============================================================== */

void
xaccSplitComputeCapGains(Split *split, Account *gain_acc)
{
   Split *opening_split;
   GNCLot *lot;
   gnc_commodity *currency = NULL;
   gnc_numeric zero = gnc_numeric_zero();
   gnc_numeric value = zero;

   if (!split) return;
   lot = split->lot;
   if (!lot) return;
   currency = split->parent->common_currency;

   ENTER ("split=%p lot=%s", split,
       kvp_frame_get_string (gnc_lot_get_slots (lot), "/title"));

   /* Make sure the status flags and pointers are initialized */
   if (GAINS_STATUS_UNKNOWN == split->gains) xaccSplitDetermineGainStatus(split);
   if (GAINS_STATUS_GAINS & split->gains)
   {
      /* If this is the split that records the gains, then work with 
       * the split that generates the gains. 
       */
      /* split = xaccSplitGetCapGainsSplit (split); */
      split = split->gains_split;

      /* This should never be NULL, and if it is, and its matching
       * parent can't be found, then its a bug, and we should be
       * discarding this split.   But ... for now .. return.
       * XXX move appropriate actions to a 'scrub' routine'
       */
      if (!split) 
      {
         PERR ("Bad gains-split pointer! .. trying to recover.");
         split->gains_split = xaccSplitGetCapGainsSplit (split);
         split = split->gains_split;
         if (!split) return;
#if MOVE_THIS_TO_A_DATA_INTEGRITY_SCRUBBER 
         xaccTransDestroy (trans);
#endif
      }
   }

   if ((FALSE == (split->gains & GAINS_STATUS_A_VDIRTY))  &&
       (split->gains_split) &&
       (FALSE == (split->gains_split->gains & GAINS_STATUS_A_VDIRTY))) return;

   /* Yow! If amount is zero, there's nothing to do! Amount-zero splits 
    * may exist if users attempted to manually record gains. */
   if (gnc_numeric_zero_p (split->amount)) return;

   opening_split = gnc_lot_get_earliest_split(lot);
   if (split == opening_split)
   {
      /* Check to make sure that this opening split doesn't 
       * have a cap-gain transaction associated with it.  
       * If it does, that's wrong, and we ruthlessly destroy it.
       * XXX Don't do this, it leads to infinite loops.
       * We need to scrub out errors like this elsewhere!
       */
#if MOVE_THIS_TO_A_DATA_INTEGRITY_SCRUBBER 
      if (xaccSplitGetCapGainsSplit (split))
      {
         Split *gains_split = xaccSplitGetCapGainsSplit(split);
         Transaction *trans = gains_split->parent;
         PERR ("Opening Split must not have cap gains!!\n");
         
         xaccTransBeginEdit (trans);
         xaccTransDestroy (trans);
         xaccTransCommitEdit (trans);
      }
#endif
      return;
   }
   
   /* Check to make sure the opening split and this split
    * use the same currency */
   if (FALSE == gnc_commodity_equiv (currency, 
                           opening_split->parent->common_currency))
   {
      /* OK, the purchase and the sale were made in different currencies.
       * I don't know how to compute cap gains for that.  This is not
       * an error. Just punt, silently. 
       */
      return;
   }

   /* Opening amount should be larger (or equal) to current split,
    * and it should be of the opposite sign.
    */
   if (0 > gnc_numeric_compare (gnc_numeric_abs(opening_split->amount),
                                gnc_numeric_abs(split->amount)))
   {
      PERR ("Malformed Lot! (too thin!)\n");
      return;
   }
   if ( (gnc_numeric_negative_p(opening_split->amount) ||
         gnc_numeric_positive_p(split->amount)) &&
        (gnc_numeric_positive_p(opening_split->amount) ||
         gnc_numeric_negative_p(split->amount)))
   {
      PERR ("Malformed Lot! (too fat!)\n");
      return;
   }

   /* The cap gains is the difference between the value of the
    * opening split, and the current split, pro-rated for an equal
    * amount of shares. 
    * i.e. purchase_price = opening_value / opening_amount 
    * cost_basis = purchase_price * current_amount
    * cap_gain = current_value - cost_basis 
    */
   value = gnc_numeric_mul (opening_split->value, split->amount,
                   GNC_DENOM_AUTO, GNC_RND_NEVER);
   value = gnc_numeric_div (value, opening_split->amount, 
                   gnc_numeric_denom(opening_split->value), GNC_DENOM_EXACT);
   
   value = gnc_numeric_sub (value, split->value,
                           GNC_DENOM_AUTO, GNC_DENOM_LCD);
   PINFO ("Open amt=%s val=%s;  split amt=%s val=%s; gains=%s\n",
          gnc_numeric_to_string (opening_split->amount),
          gnc_numeric_to_string (opening_split->value),
          gnc_numeric_to_string (split->amount),
          gnc_numeric_to_string (split->value),
          gnc_numeric_to_string (value));

   /* Are the cap gains zero?  If not, add a balancing transaction.
    * As per design doc lots.txt: the transaction has two splits, 
    * with equal & opposite values.  The amt of one iz zero (so as
    * not to upset the lot balance), the amt of the other is the same 
    * as its value (its the realized gain/loss).
    */
   if (FALSE == gnc_numeric_equal (value, zero))
   {
      Transaction *trans;
      Split *lot_split, *gain_split;
      Timespec ts;

      /* See if there already is an associated gains transaction.
       * If there is, adjust its value as appropriate. Else, create 
       * a new gains transaction.
       */
      lot_split = xaccSplitGetCapGainsSplit (split);

      if (NULL == lot_split)
      {
         Account *lot_acc = lot->account;
         QofBook *book = lot_acc->book;

         lot_split = xaccMallocSplit (book);
         gain_split = xaccMallocSplit (book);

         /* Check to make sure the gains account currency matches. */
         if ((NULL == gain_acc) ||
             (FALSE == gnc_commodity_equiv (currency,
                                   xaccAccountGetCommodity(gain_acc))))
         {
            gain_acc = GetOrMakeGainAcct (lot_acc, currency);
         }

         xaccAccountBeginEdit (gain_acc);
         xaccAccountInsertSplit (gain_acc, gain_split);
         xaccAccountCommitEdit (gain_acc);

         xaccAccountBeginEdit (lot_acc);
         xaccAccountInsertSplit (lot_acc, lot_split);
         xaccAccountCommitEdit (lot_acc);

         trans = xaccMallocTransaction (book);

         xaccTransBeginEdit (trans);
         xaccTransSetCurrency (trans, currency);
         xaccTransSetDescription (trans, _("Realized Gain/Loss"));
         
         xaccTransAppendSplit (trans, lot_split);
         xaccTransAppendSplit (trans, gain_split);

         xaccSplitSetMemo (lot_split, _("Realized Gain/Loss"));
         xaccSplitSetMemo (gain_split, _("Realized Gain/Loss"));

         /* For the new transaction, install KVP markup indicating 
          * that this is the gains transaction that corresponds
          * to the gains source.
          */
         kvp_frame_set_guid (split->kvp_data, "gains-split", 
                     xaccSplitGetGUID (lot_split));
         kvp_frame_set_guid (lot_split->kvp_data, "gains-source", 
                     xaccSplitGetGUID (split));

      }
      else
      {
         trans = lot_split->parent;
         gain_split = xaccSplitGetOtherSplit (lot_split);
         xaccTransBeginEdit (trans);

         /* Make sure the existing gains trans has the correct currency,
          * just in case someone screwed with it! */
         if (FALSE == gnc_commodity_equiv(currency,trans->common_currency))
         {
            xaccTransSetCurrency (trans, currency);
         }
      }

      /* Common to both */
      ts = xaccTransRetDatePostedTS (split->parent);
      xaccTransSetDatePostedTS (trans, &ts);
      xaccTransSetDateEnteredSecs (trans, time(0));

      xaccSplitSetAmount (lot_split, zero);
      xaccSplitSetValue (lot_split, value);
      gnc_lot_add_split (lot, lot_split);

      value = gnc_numeric_neg (value);
      xaccSplitSetAmount (gain_split, value);
      xaccSplitSetValue (gain_split, value);

      /* Some short-cuts to help avoid the above kvp lookup. */
      split->gains = GAINS_STATUS_CLEAN;
      split->gains_split = lot_split;
      lot_split->gains = GAINS_STATUS_GAINS;
      lot_split->gains_split = split;
      gain_split->gains = GAINS_STATUS_GAINS;
      gain_split->gains_split = split;

      xaccTransCommitEdit (trans);
   }
   LEAVE ("lot=%s", kvp_frame_get_string (gnc_lot_get_slots (lot), "/title"));
}

/* ============================================================== */

gnc_numeric 
xaccSplitGetCapGains(Split * split)
{
   if (!split) return gnc_numeric_zero();

   if (GAINS_STATUS_UNKNOWN == split->gains) xaccSplitDetermineGainStatus(split);
   if ((split->gains & GAINS_STATUS_A_VDIRTY) || 
       (split->gains_split && (split->gains_split->gains & GAINS_STATUS_A_VDIRTY)))
   {
      xaccSplitComputeCapGains (split, NULL);
   }

   /* If this is the source split, get the gains from the one 
    * that records the gains.  If this already is the gains split, 
    * its a no-op. */
   if (!(GAINS_STATUS_GAINS & split->gains))
   {
      /* split = xaccSplitGetCapGainsSplit (split); */
      split = split->gains_split;
   }

   if (!split) return gnc_numeric_zero();

   return split->value;
}

/* =========================== END OF FILE ======================= */