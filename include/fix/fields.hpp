#pragma once

namespace hftob {
namespace fix {

/// FIX (Financial Information eXchange) is the order-entry protocol every trading
/// venue speaks — the counterpart to a market-data feed like ITCH. Messages are
/// `tag=value` pairs delimited by SOH (0x01), framed by `8=BeginString`,
/// `9=BodyLength`, and a trailing `10=CheckSum`. This is a compact FIX 4.4 codec
/// for the order-entry path (NewOrderSingle / ExecutionReport).
constexpr char SOH = '\x01';

namespace tag {
constexpr int BeginString  = 8;
constexpr int BodyLength   = 9;
constexpr int CheckSum     = 10;
constexpr int MsgType      = 35;
constexpr int SenderCompID = 49;
constexpr int TargetCompID = 56;
constexpr int MsgSeqNum    = 34;
constexpr int SendingTime  = 52;
// order entry
constexpr int ClOrdID      = 11;
constexpr int OrderID      = 37;
constexpr int ExecID       = 17;
constexpr int Symbol       = 55;
constexpr int Side         = 54;
constexpr int OrderQty     = 38;
constexpr int OrdType      = 40;
constexpr int Price        = 44;
constexpr int TimeInForce  = 59;
constexpr int TransactTime = 60;
// execution report
constexpr int ExecType     = 150;
constexpr int OrdStatus    = 39;
constexpr int LeavesQty    = 151;
constexpr int CumQty       = 14;
constexpr int AvgPx        = 6;
constexpr int LastPx       = 31;
constexpr int LastQty      = 32;
constexpr int Text         = 58;
}  // namespace tag

// On-wire FIX enum values (the literal characters sent for these tags).
enum class Side : char { Buy = '1', Sell = '2' };
enum class OrdType : char { Market = '1', Limit = '2' };
enum class TimeInForce : char { Day = '0', IOC = '3', FOK = '4', GTC = '1' };
enum class ExecType : char { New = '0', PartialFill = '1', Fill = '2', Canceled = '4', Rejected = '8' };
enum class OrdStatus : char { New = '0', PartiallyFilled = '1', Filled = '2', Canceled = '4', Rejected = '8' };

}  // namespace fix
}  // namespace hftob
