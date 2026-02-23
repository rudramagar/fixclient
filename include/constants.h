#ifndef CONSTANTS_H
#define CONSTANTS_H

static const char* const RED = "\x1b[31m";
static const char* const GREEN = "\x1b[32m";
static const char* const RESET = "\x1b[0m";

static const int fix_tag_begin_string                   = 8;
static const int fix_tag_body_length                    = 9;
static const int fix_tag_check_sum                      = 10;
static const int fix_tag_msg_seq_num                    = 34;
static const int fix_tag_msg_type                       = 35;
static const int fix_tag_sender_comp_id                 = 49;
static const int fix_tag_sender_sub_id                  = 50;
static const int fix_tag_sending_time                   = 52;
static const int fix_tag_target_comp_id                 = 56;
static const int fix_tag_transact_time                  = 60;
static const int fix_tag_ref_seq_num                    = 45;
static const int fix_tag_ref_tag_id                     = 371;
static const int fix_tag_ref_msg_type                   = 372;
static const int fix_tag_sess_rej_reason                = 373;
static const int fix_tag_clord_id                       = 11;
static const int fix_tag_order_id                       = 37;
static const int fix_tag_exec_id                        = 17;
static const int fix_tag_avg_px                         = 6;
static const int fix_tag_cum_qty                        = 14;
static const int fix_tag_leaves_qty                     = 151;
static const int fix_tag_order_qty                      = 38;
static const int fix_tag_ord_status                     = 39;
static const int fix_tag_ord_type                       = 40;
static const int fix_tag_price                          = 44;
static const int fix_tag_side                           = 54;
static const int fix_tag_symbol                         = 55;
static const int fix_tag_text                           = 58;
static const int fix_tag_exec_type                      = 150;
static const int fix_tag_ord_rej_reason                 = 103;
static const int fix_tag_settl_type                     = 63;
static const int fix_tag_order_capacity                 = 528;
static const int fix_tag_cash_order_qty                 = 544;
static const int fix_tag_cross_id                       = 548;
static const int fix_tag_no_sides                       = 552;
static const int fix_tag_order_classification           = 8060;
static const int fix_tag_dark_pool_flag                 = 8062;

// Tags Lookup
static inline const char* fix_tag_name(int tag) {
    switch(tag) {
        case fix_tag_begin_string:                  return "BeginString";
        case fix_tag_body_length:                   return "BodyLength";
        case fix_tag_check_sum:                     return "CheckSum";
        case fix_tag_msg_seq_num:                   return "MsgSeqNum";
        case fix_tag_msg_type:                      return "MsgType";
        case fix_tag_sender_comp_id:                return "SenderCompID";
        case fix_tag_sender_sub_id:                 return "SenderSubID";
        case fix_tag_sending_time:                  return "SendingTime";
        case fix_tag_target_comp_id:                return "TargetCompID";
        case fix_tag_transact_time:                 return "TransactTime";
        case fix_tag_ref_seq_num:                   return "RefSeqNum";
        case fix_tag_ref_tag_id:                    return "RefTagID";
        case fix_tag_ref_msg_type:                  return "RefMsgType";
        case fix_tag_sess_rej_reason:               return "SessionRejectReason";
        case fix_tag_clord_id:                      return "ClOrdID";
        case fix_tag_order_id:                      return "OrderID";
        case fix_tag_exec_id:                       return "ExecID";
        case fix_tag_avg_px:                        return "AvgPx";
        case fix_tag_cum_qty:                       return "CumQty";
        case fix_tag_leaves_qty:                    return "LeavesQty";
        case fix_tag_order_qty:                     return "OrderQty";
        case fix_tag_ord_status:                    return "OrdStatus";
        case fix_tag_ord_type:                      return "OrdType";
        case fix_tag_price:                         return "Price";
        case fix_tag_side:                          return "Side";
        case fix_tag_symbol:                        return "Symbol";
        case fix_tag_text:                          return "Text";
        case fix_tag_exec_type:                     return "ExecType";
        case fix_tag_ord_rej_reason:                return "OrdRejReason";
        case fix_tag_settl_type:                    return "SettlType";
        case fix_tag_order_capacity:                return "OrderCapacity";
        case fix_tag_cash_order_qty:                return "CashOrderQty";
        case fix_tag_cross_id:                      return "CrossID";
        case fix_tag_no_sides:                      return "NoSides";
        case fix_tag_order_classification:          return "OrderClassification";
        case fix_tag_dark_pool_flag:                return "DarkPoolFlag";
        default:                                    return 0;
    }
}

#endif
