//+------------------------------------------------------------------+
//|                                                  ITCHBridge.mq5   |
//|   MetaTrader 5 <-> hft-orderbook bridge (client side)            |
//|                                                                  |
//|   Streams this symbol's live ticks to the `mt5d` engine as       |
//|   NDJSON, reads back order commands, executes them with          |
//|   OrderSend(), and returns the MqlTradeResult retcode as an ack. |
//|                                                                  |
//|   Transport: MQL5 built-in Socket* API (no DLLs, no ZeroMQ).     |
//|   This .mq5 is a source artifact — it is compiled in MetaEditor  |
//|   on Windows, not by this repo's CI (which is Linux-only). The   |
//|   C++ side and the wire protocol are verified in CI by mt5d +    |
//|   the mock-client integration test.                              |
//+------------------------------------------------------------------+
#property copyright "Muhammad Saad"
#property version   "1.00"
#property strict

input string InpHost = "127.0.0.1";   // mt5d host
input int    InpPort = 9009;          // mt5d port
input double InpHeartbeatSecs = 5.0;  // heartbeat interval

int      g_sock = INVALID_HANDLE;
string   g_rxbuf = "";                // partial-line receive buffer
datetime g_last_hb = 0;

//+------------------------------------------------------------------+
//| Low-level send: append a single NDJSON line over the socket      |
//+------------------------------------------------------------------+
bool SendLine(const string line)
  {
   if(g_sock == INVALID_HANDLE)
      return(false);
   uchar data[];
   int len = StringToCharArray(line, data, 0, WHOLE_ARRAY, CP_UTF8);
   // StringToCharArray appends a trailing 0 terminator; do not send it.
   if(len > 0 && data[len-1] == 0)
      len--;
   return(SocketSend(g_sock, data, len) == len);
  }

//+------------------------------------------------------------------+
//| Pull whatever is available and return one line if buffered       |
//+------------------------------------------------------------------+
bool RecvLine(string &out)
  {
   // Drain socket into the string buffer.
   uint avail = SocketIsReadable(g_sock);
   while(avail > 0)
     {
      uchar chunk[];
      int got = SocketRead(g_sock, chunk, (int)avail, 50);
      if(got <= 0)
         break;
      g_rxbuf += CharArrayToString(chunk, 0, got, CP_UTF8);
      avail = SocketIsReadable(g_sock);
     }
   int nl = StringFind(g_rxbuf, "\n");
   if(nl < 0)
      return(false);
   out = StringSubstr(g_rxbuf, 0, nl);
   g_rxbuf = StringSubstr(g_rxbuf, nl + 1);
   return(true);
  }

//+------------------------------------------------------------------+
//| Tiny flat-JSON field readers (match the C++ codec's schema)      |
//+------------------------------------------------------------------+
bool JsonStr(const string s, const string key, string &out)
  {
   int k = StringFind(s, "\"" + key + "\"");
   if(k < 0) return(false);
   int c = StringFind(s, ":", k);
   if(c < 0) return(false);
   int q1 = StringFind(s, "\"", c + 1);
   if(q1 < 0) return(false);
   int q2 = StringFind(s, "\"", q1 + 1);
   if(q2 < 0) return(false);
   out = StringSubstr(s, q1 + 1, q2 - q1 - 1);
   return(true);
  }

bool JsonNum(const string s, const string key, double &out)
  {
   int k = StringFind(s, "\"" + key + "\"");
   if(k < 0) return(false);
   int c = StringFind(s, ":", k);
   if(c < 0) return(false);
   out = StringToDouble(StringSubstr(s, c + 1));
   return(true);
  }

//+------------------------------------------------------------------+
//| Encode the current tick as an NDJSON line                        |
//+------------------------------------------------------------------+
string EncodeTick(const MqlTick &t)
  {
   string s = "{\"t\":\"tick\",\"v\":1";
   s += ",\"symbol\":\"" + _Symbol + "\"";
   s += ",\"time\":" + IntegerToString((long)t.time);
   s += ",\"bid\":"  + DoubleToString(t.bid, _Digits);
   s += ",\"ask\":"  + DoubleToString(t.ask, _Digits);
   s += ",\"last\":" + DoubleToString(t.last, _Digits);
   s += ",\"volume\":" + IntegerToString((long)t.volume);
   s += "}\n";
   return(s);
  }

//+------------------------------------------------------------------+
//| Execute an order command and return its retcode as an ack line   |
//+------------------------------------------------------------------+
string ExecuteOrder(const string line)
  {
   long   id = (long)0;
   double idd; if(JsonNum(line, "id", idd)) id = (long)idd;
   string side; JsonStr(line, "side", side);
   double volume = 0.0; JsonNum(line, "volume", volume);

   MqlTradeRequest req;  ZeroMemory(req);
   MqlTradeResult  res;  ZeroMemory(res);
   req.action   = TRADE_ACTION_DEAL;
   req.symbol   = _Symbol;
   req.volume   = (volume > 0 ? volume : 0.10);
   req.type     = (side == "B" ? ORDER_TYPE_BUY : ORDER_TYPE_SELL);
   req.price    = (side == "B" ? SymbolInfoDouble(_Symbol, SYMBOL_ASK)
                               : SymbolInfoDouble(_Symbol, SYMBOL_BID));
   req.deviation = 20;
   req.type_filling = ORDER_FILLING_IOC;

   bool ok = OrderSend(req, res);
   string ackmsg = (ok ? "sent" : "rejected");
   string ack = "{\"t\":\"ack\",\"v\":1";
   ack += ",\"id\":" + IntegerToString(id);
   ack += ",\"ok\":" + (res.retcode == TRADE_RETCODE_DONE ? "true" : "false");
   ack += ",\"retcode\":" + IntegerToString((int)res.retcode);
   ack += ",\"message\":\"" + ackmsg + "\"}\n";
   return(ack);
  }

//+------------------------------------------------------------------+
int OnInit()
  {
   g_sock = SocketCreate();
   if(g_sock == INVALID_HANDLE)
     {
      Print("SocketCreate failed: ", GetLastError());
      return(INIT_FAILED);
     }
   if(!SocketConnect(g_sock, InpHost, InpPort, 3000))
     {
      Print("SocketConnect to ", InpHost, ":", InpPort, " failed: ", GetLastError());
      return(INIT_FAILED);
     }
   SendLine("{\"t\":\"hello\",\"v\":1,\"client\":\"ITCHBridge.mq5\",\"account\":"
            + IntegerToString((long)AccountInfoInteger(ACCOUNT_LOGIN)) + "}\n");
   SendLine("{\"t\":\"subscribe\",\"v\":1,\"symbol\":\"" + _Symbol + "\"}\n");
   EventSetTimer(1);
   Print("ITCHBridge connected to ", InpHost, ":", InpPort);
   return(INIT_SUCCEEDED);
  }

//+------------------------------------------------------------------+
void OnDeinit(const int reason)
  {
   EventKillTimer();
   if(g_sock != INVALID_HANDLE)
     {
      SendLine("{\"t\":\"bye\",\"v\":1}\n");
      SocketClose(g_sock);
      g_sock = INVALID_HANDLE;
     }
  }

//+------------------------------------------------------------------+
//| Stream each new tick; act on any order replies                   |
//+------------------------------------------------------------------+
void OnTick()
  {
   if(g_sock == INVALID_HANDLE)
      return;
   MqlTick t;
   if(!SymbolInfoTick(_Symbol, t))
      return;
   SendLine(EncodeTick(t));

   // The engine replies to every tick (order or nop); drain replies.
   string line;
   while(RecvLine(line))
     {
      if(StringFind(line, "\"t\":\"order\"") >= 0)
         SendLine(ExecuteOrder(line));
      // "nop" and others require no action.
     }
  }

//+------------------------------------------------------------------+
//| Heartbeat + read any pending replies between ticks               |
//+------------------------------------------------------------------+
void OnTimer()
  {
   if(g_sock == INVALID_HANDLE)
      return;
   datetime now = TimeCurrent();
   if(now - g_last_hb >= (datetime)InpHeartbeatSecs)
     {
      SendLine("{\"t\":\"heartbeat\",\"v\":1,\"time\":" + IntegerToString((long)now) + "}\n");
      g_last_hb = now;
     }
   string line;
   while(RecvLine(line))
      if(StringFind(line, "\"t\":\"order\"") >= 0)
         SendLine(ExecuteOrder(line));
  }
//+------------------------------------------------------------------+
