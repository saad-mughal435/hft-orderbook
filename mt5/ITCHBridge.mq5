//+------------------------------------------------------------------+
//|                                                  ITCHBridge.mq5   |
//|   MetaTrader 5 <-> hft-orderbook bridge (client side)            |
//|                                                                  |
//|   Streams this symbol's live ticks to the `mt5d` engine as       |
//|   NDJSON, reads back order commands, executes them with          |
//|   OrderSend(), and returns the MqlTradeResult retcode as an ack. |
//|                                                                  |
//|   Also renders an on-chart execution-bridge terminal (HUD): live |
//|   top-of-book, a mid sparkline, I/O throughput, the last         |
//|   execution, and link/heartbeat status. The HUD is additive and  |
//|   draws only real bridge data - it never fabricates depth.       |
//|                                                                  |
//|   Transport: MQL5 built-in Socket* API (no DLLs, no ZeroMQ).     |
//|   This .mq5 is a source artifact - it is compiled in MetaEditor  |
//|   on Windows, not by this repo's CI (which is Linux-only). The   |
//|   C++ side and the wire protocol are verified in CI by mt5d +    |
//|   the mock-client integration test.                              |
//+------------------------------------------------------------------+
#property copyright "Muhammad Saad"
#property version   "1.10"
#property strict

input string InpHost          = "127.0.0.1";  // mt5d host
input int    InpPort          = 9009;         // mt5d port
input double InpHeartbeatSecs = 5.0;          // heartbeat interval
input bool   InpShowPanel     = true;         // draw the on-chart HUD

int      g_sock = INVALID_HANDLE;
string   g_rxbuf = "";                // partial-line receive buffer
datetime g_last_hb = 0;

//--- HUD state (all derived from real bridge activity) --------------
long     g_ticks   = 0;               // ticks streamed to the engine
long     g_orders  = 0;               // order commands received
long     g_acks_ok = 0;               // executions that filled
long     g_acks_rej= 0;               // executions rejected
long     g_nops    = 0;               // no-op replies
double   g_bid=0.0, g_ask=0.0, g_last=0.0;
long     g_vol     = 0;
datetime g_started = 0;
uint     g_last_ms = 0;               // GetTickCount() of previous tick
double   g_rate    = 0.0;             // smoothed ticks/sec
int      g_status  = 0;               // 0 down, 1 linking, 2 live
string   g_exec    = "no executions yet";
color    g_execclr = clrSilver;
double   g_mid[48];                   // mid-price ring for the sparkline
int      g_midn    = 0;

//--- design tokens (skill: Stock/Trading OHLC + Fintech dark) -------
#define PFX        "HOB_"             // object-name prefix (one namespace)
#define C_BG       C'15,23,42'        // #0F172A slate-950
#define C_SURF     C'27,37,58'        // #1B253A header / chips
#define C_BORDER   C'51,65,85'        // #334155 slate-700
#define C_TEXT     C'226,232,240'     // #E2E8F0 slate-200
#define C_MUTE     C'129,144,166'     // muted slate (>=4.5:1 on bg)
#define C_BID      C'38,166,154'      // #26A69A bullish/bid
#define C_ASK      C'239,83,80'       // #EF5350 bearish/ask
#define C_AMBER    C'245,158,11'      // #F59E0B accent / linking
#define C_SPARK    C'56,116,150'      // calm cyan-slate for the sparkline

//--- panel geometry (top-left, floated off the edge) ---------------
#define PX   14
#define PY   30
#define PW   304
#define PH   312
#define LX   (PX+14)                  // content left
#define RX   (PX+PW-14)               // content right (right-anchored values)
#define CX   (PX+PW/2)                // column split

//+------------------------------------------------------------------+
//| Object helpers                                                   |
//+------------------------------------------------------------------+
void Rect(const string n,int x,int y,int w,int h,color bg,color brd)
  {
   string nm=PFX+n;
   if(ObjectFind(0,nm)<0) ObjectCreate(0,nm,OBJ_RECTANGLE_LABEL,0,0,0);
   ObjectSetInteger(0,nm,OBJPROP_CORNER,CORNER_LEFT_UPPER);
   ObjectSetInteger(0,nm,OBJPROP_XDISTANCE,x);
   ObjectSetInteger(0,nm,OBJPROP_YDISTANCE,y);
   ObjectSetInteger(0,nm,OBJPROP_XSIZE,w);
   ObjectSetInteger(0,nm,OBJPROP_YSIZE,h);
   ObjectSetInteger(0,nm,OBJPROP_BGCOLOR,bg);
   ObjectSetInteger(0,nm,OBJPROP_BORDER_TYPE,BORDER_FLAT);
   ObjectSetInteger(0,nm,OBJPROP_COLOR,brd);
   ObjectSetInteger(0,nm,OBJPROP_BACK,false);
   ObjectSetInteger(0,nm,OBJPROP_SELECTABLE,false);
   ObjectSetInteger(0,nm,OBJPROP_HIDDEN,true);
  }

void Label(const string n,int x,int y,const string txt,color clr,int sz,
           const string font,ENUM_ANCHOR_POINT anchor)
  {
   string nm=PFX+n;
   if(ObjectFind(0,nm)<0) ObjectCreate(0,nm,OBJ_LABEL,0,0,0);
   ObjectSetInteger(0,nm,OBJPROP_CORNER,CORNER_LEFT_UPPER);
   ObjectSetInteger(0,nm,OBJPROP_ANCHOR,anchor);
   ObjectSetInteger(0,nm,OBJPROP_XDISTANCE,x);
   ObjectSetInteger(0,nm,OBJPROP_YDISTANCE,y);
   ObjectSetString (0,nm,OBJPROP_FONT,font);
   ObjectSetInteger(0,nm,OBJPROP_FONTSIZE,sz);
   ObjectSetInteger(0,nm,OBJPROP_COLOR,clr);
   ObjectSetString (0,nm,OBJPROP_TEXT,txt);
   ObjectSetInteger(0,nm,OBJPROP_BACK,false);
   ObjectSetInteger(0,nm,OBJPROP_SELECTABLE,false);
   ObjectSetInteger(0,nm,OBJPROP_HIDDEN,true);
  }

void SetTxt(const string n,const string txt,color clr)
  {
   string nm=PFX+n;
   ObjectSetString (0,nm,OBJPROP_TEXT,txt);
   ObjectSetInteger(0,nm,OBJPROP_COLOR,clr);
  }

//--- thousands separator for counters ------------------------------
string Grp(long v)
  {
   string s=IntegerToString(v<0?-v:v), out="";
   int c=0;
   for(int i=StringLen(s)-1;i>=0;i--)
     {
      out=StringSubstr(s,i,1)+out;
      if(++c%3==0 && i>0) out=","+out;
     }
   return((v<0?"-":"")+out);
  }

string HMS(int secs)
  {
   if(secs<0) secs=0;
   int h=secs/3600, m=(secs%3600)/60, s=secs%60;
   return(StringFormat("%02d:%02d:%02d",h,m,s));
  }

//+------------------------------------------------------------------+
//| Build the static HUD chrome once                                 |
//+------------------------------------------------------------------+
void PanelBuild()
  {
   if(!InpShowPanel) return;
   Rect("bg",      PX,    PY,    PW, PH, C_BG,   C_BORDER);
   Rect("hd",      PX,    PY,    PW, 32, C_SURF, C_BORDER);
   Rect("accent",  PX,    PY,    3,  32, C_AMBER,C_AMBER);   // brand spine
   Rect("dot",     RX-58, PY+12, 8,  8,  C_AMBER,C_AMBER);   // status dot

   Label("title", LX, PY+9,  "HFT ORDER-BOOK BRIDGE", C_TEXT, 10, "Segoe UI Semibold", ANCHOR_LEFT_UPPER);
   Label("st",    RX, PY+9,  "LINKING",               C_AMBER, 8, "Segoe UI Semibold", ANCHOR_RIGHT_UPPER);
   Label("sub",   LX, PY+40, "-",                     C_MUTE,  8, "Consolas",          ANCHOR_LEFT_UPPER);

   // Top of book
   Rect("d1", PX+12, PY+62, PW-24, 1, C_BORDER, C_BORDER);
   Label("kbid", LX,   PY+70,  "BID", C_MUTE, 8, "Segoe UI", ANCHOR_LEFT_UPPER);
   Label("kask", CX+4, PY+70,  "ASK", C_MUTE, 8, "Segoe UI", ANCHOR_LEFT_UPPER);
   Label("vbid", LX,   PY+82,  "-",   C_BID, 13, "Consolas", ANCHOR_LEFT_UPPER);
   Label("vask", CX+4, PY+82,  "-",   C_ASK, 13, "Consolas", ANCHOR_LEFT_UPPER);
   Label("vspr", LX,   PY+108, "SPREAD   -", C_TEXT, 9, "Consolas", ANCHOR_LEFT_UPPER);
   Label("vmid", CX+4, PY+108, "MID -",      C_TEXT, 9, "Consolas", ANCHOR_LEFT_UPPER);
   Label("vlast",LX,   PY+124, "LAST -",     C_MUTE, 9, "Consolas", ANCHOR_LEFT_UPPER);
   Label("vvol", CX+4, PY+124, "VOL  -",     C_MUTE, 9, "Consolas", ANCHOR_LEFT_UPPER);

   // Mid sparkline
   Rect("d2", PX+12, PY+146, PW-24, 1, C_BORDER, C_BORDER);
   Label("kmid", LX, PY+152, "MID  trend", C_MUTE, 8, "Segoe UI", ANCHOR_LEFT_UPPER);
   for(int i=0;i<24;i++)
      Rect("sp"+IntegerToString(i), LX+i*11, PY+186, 8, 2, C_SPARK, C_SPARK);

   // Throughput
   Rect("d3", PX+12, PY+196, PW-24, 1, C_BORDER, C_BORDER);
   Label("ticks", LX,   PY+204, "TICKS  -",  C_TEXT, 9, "Consolas", ANCHOR_LEFT_UPPER);
   Label("rate",  CX+4, PY+204, "RATE  -",   C_TEXT, 9, "Consolas", ANCHOR_LEFT_UPPER);
   Label("ord",   LX,   PY+220, "ORDERS -",  C_TEXT, 9, "Consolas", ANCHOR_LEFT_UPPER);
   Label("ack",   CX+4, PY+220, "ACK -",     C_TEXT, 9, "Consolas", ANCHOR_LEFT_UPPER);

   // Last execution
   Rect("d4", PX+12, PY+242, PW-24, 1, C_BORDER, C_BORDER);
   Label("kexec", LX, PY+248, "LAST EXECUTION", C_MUTE, 8, "Segoe UI", ANCHOR_LEFT_UPPER);
   Label("exec",  LX, PY+262, g_exec, g_execclr, 9, "Consolas", ANCHOR_LEFT_UPPER);

   // Footer
   Rect("d5", PX+12, PY+284, PW-24, 1, C_BORDER, C_BORDER);
   Label("foot", LX, PY+290, "-",        C_MUTE, 8, "Consolas", ANCHOR_LEFT_UPPER);
   Label("up",   RX, PY+290, "UP 00:00:00", C_MUTE, 8, "Consolas", ANCHOR_RIGHT_UPPER);
  }

void PanelDestroy() { ObjectsDeleteAll(0,PFX); }

//+------------------------------------------------------------------+
//| Refresh the dynamic HUD values                                   |
//+------------------------------------------------------------------+
void PanelUpdate()
  {
   if(!InpShowPanel) return;

   // status pill + dot
   string stxt; color sclr;
   if(g_status==2)      { stxt="LIVE";    sclr=C_BID;   }
   else if(g_status==1) { stxt="LINKING"; sclr=C_AMBER; }
   else                 { stxt="OFFLINE"; sclr=C_ASK;   }
   SetTxt("st",stxt,sclr);
   ObjectSetInteger(0,PFX+"dot",OBJPROP_BGCOLOR,sclr);
   ObjectSetInteger(0,PFX+"dot",OBJPROP_COLOR,sclr);

   SetTxt("sub", _Symbol+"  #"+IntegerToString((long)AccountInfoInteger(ACCOUNT_LOGIN))
                 +"  "+InpHost+":"+IntegerToString(InpPort), C_MUTE);

   int dg = (int)_Digits;
   SetTxt("vbid", g_bid>0 ? DoubleToString(g_bid,dg) : "-", C_BID);
   SetTxt("vask", g_ask>0 ? DoubleToString(g_ask,dg) : "-", C_ASK);

   if(g_bid>0 && g_ask>0)
     {
      double pts = (g_ask-g_bid)/_Point;
      SetTxt("vspr", StringFormat("SPREAD %5.0f", pts), C_TEXT);
      SetTxt("vmid", "MID "+DoubleToString((g_bid+g_ask)/2.0,dg), C_TEXT);
     }
   SetTxt("vlast", "LAST "+(g_last>0?DoubleToString(g_last,dg):"-"), C_MUTE);
   SetTxt("vvol",  "VOL  "+Grp(g_vol), C_MUTE);

   SetTxt("ticks", "TICKS  "+Grp(g_ticks), C_TEXT);
   SetTxt("rate",  StringFormat("RATE %5.1f/s", g_rate), C_TEXT);
   SetTxt("ord",   "ORDERS "+Grp(g_orders), C_TEXT);
   SetTxt("ack",   StringFormat("ACK %d ok / %d rej", (int)g_acks_ok, (int)g_acks_rej),
                   (g_acks_rej>0 ? C_AMBER : C_TEXT));
   SetTxt("exec",  g_exec, g_execclr);

   SetTxt("foot", "mt5d - NDJSON - proto v1", C_MUTE);
   if(g_started>0)
      SetTxt("up", "UP "+HMS((int)(TimeCurrent()-g_started)), C_MUTE);

   // sparkline: last 24 mids, normalised to bar heights 2..26
   int n = (g_midn<24 ? g_midn : 24);
   double lo=0,hi=0; bool first=true;
   for(int i=0;i<n;i++)
     {
      double v=g_mid[(g_midn-n+i+48)%48];
      if(first){lo=hi=v;first=false;}
      if(v<lo)lo=v; if(v>hi)hi=v;
     }
   double rng=(hi-lo); if(rng<=0) rng=1;
   for(int i=0;i<24;i++)
     {
      string nm="sp"+IntegerToString(i);
      if(i<n)
        {
         double v=g_mid[(g_midn-n+i+48)%48];
         int h=2+(int)((v-lo)/rng*20.0);  // 2..22 px, clears the label above
         color c=(i>0 && v>=g_mid[(g_midn-n+i-1+48)%48]) ? C_BID : C_SPARK;
         ObjectSetInteger(0,PFX+nm,OBJPROP_YDISTANCE,PY+188-h);  // bottom-aligned at PY+188
         ObjectSetInteger(0,PFX+nm,OBJPROP_YSIZE,h);
         ObjectSetInteger(0,PFX+nm,OBJPROP_BGCOLOR,c);
         ObjectSetInteger(0,PFX+nm,OBJPROP_COLOR,c);
        }
      else
        {
         ObjectSetInteger(0,PFX+nm,OBJPROP_YDISTANCE,PY+186);
         ObjectSetInteger(0,PFX+nm,OBJPROP_YSIZE,2);
         ObjectSetInteger(0,PFX+nm,OBJPROP_BGCOLOR,C_SURF);
         ObjectSetInteger(0,PFX+nm,OBJPROP_COLOR,C_SURF);
        }
     }
   ChartRedraw(0);
  }

//--- record one tick into HUD state --------------------------------
void HudOnTick(const MqlTick &t)
  {
   g_ticks++;
   g_bid=t.bid; g_ask=t.ask; g_last=t.last; g_vol=(long)t.volume;
   if(t.bid>0 && t.ask>0)
     {
      g_mid[g_midn%48]=(t.bid+t.ask)/2.0;
      g_midn++;
     }
   uint now=GetTickCount();
   if(g_last_ms>0 && now>g_last_ms)
     {
      double inst=1000.0/(double)(now-g_last_ms);
      g_rate = (g_rate<=0 ? inst : g_rate*0.8+inst*0.2);  // EMA
     }
   g_last_ms=now;
  }

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

   // HUD: record this execution
   bool done = (res.retcode == TRADE_RETCODE_DONE);
   if(done) g_acks_ok++; else g_acks_rej++;
   g_exec = StringFormat("%s %.2f  ->  %s  rc%u",
                         (side=="B"?"BUY ":"SELL"),
                         (volume>0?volume:0.10),
                         (done?"DONE":"REJECT"), res.retcode);
   g_execclr = (done ? C_BID : C_ASK);

   string ack = "{\"t\":\"ack\",\"v\":1";
   ack += ",\"id\":" + IntegerToString(id);
   ack += ",\"ok\":" + (res.retcode == TRADE_RETCODE_DONE ? "true" : "false");
   ack += ",\"retcode\":" + IntegerToString((int)res.retcode);
   ack += ",\"message\":\"" + ackmsg + "\"}\n";
   return(ack);
  }

//+------------------------------------------------------------------+
//| (Re)connect to mt5d and re-announce hello + subscribe            |
//+------------------------------------------------------------------+
bool Connect()
  {
   g_status = 1;   // linking
   if(g_sock != INVALID_HANDLE)
      SocketClose(g_sock);
   g_rxbuf = "";
   g_sock = SocketCreate();
   if(g_sock == INVALID_HANDLE)
     {
      Print("SocketCreate failed: ", GetLastError());
      g_status = 0;
      return(false);
     }
   if(!SocketConnect(g_sock, InpHost, InpPort, 3000))
     {
      Print("SocketConnect to ", InpHost, ":", InpPort, " failed: ", GetLastError());
      SocketClose(g_sock);
      g_sock = INVALID_HANDLE;
      g_status = 0;
      return(false);
     }
   SendLine("{\"t\":\"hello\",\"v\":1,\"client\":\"ITCHBridge.mq5\",\"account\":"
            + IntegerToString((long)AccountInfoInteger(ACCOUNT_LOGIN)) + "}\n");
   SendLine("{\"t\":\"subscribe\",\"v\":1,\"symbol\":\"" + _Symbol + "\"}\n");
   Print("ITCHBridge connected to ", InpHost, ":", InpPort);
   g_status = 2;   // live
   return(true);
  }

//+------------------------------------------------------------------+
int OnInit()
  {
   g_started = TimeCurrent();
   PanelBuild();
   PanelUpdate();
   EventSetTimer(1);
   if(!Connect())
      Print("ITCHBridge: initial connect failed; will retry on timer");
   PanelUpdate();
   return(INIT_SUCCEEDED);   // keep the EA alive so OnTimer can reconnect
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
   PanelDestroy();
  }

//+------------------------------------------------------------------+
//| Stream each new tick; act on any order replies                   |
//+------------------------------------------------------------------+
void OnTick()
  {
   if(g_sock == INVALID_HANDLE || !SocketIsConnected(g_sock))
     {                          // OnTimer owns (re)connection
      g_status = (g_sock==INVALID_HANDLE ? 0 : 1);
      PanelUpdate();
      return;
     }
   MqlTick t;
   if(!SymbolInfoTick(_Symbol, t))
      return;
   SendLine(EncodeTick(t));
   HudOnTick(t);

   // The engine replies to every tick (order or nop); drain replies.
   string line;
   while(RecvLine(line))
     {
      if(StringFind(line, "\"t\":\"order\"") >= 0)
        {
         g_orders++;
         SendLine(ExecuteOrder(line));
        }
      else if(StringFind(line, "\"t\":\"nop\"") >= 0)
         g_nops++;
      // other message types require no action.
     }
   PanelUpdate();
  }

//+------------------------------------------------------------------+
//| Heartbeat + read any pending replies between ticks               |
//+------------------------------------------------------------------+
void OnTimer()
  {
   // Reconnect if the link dropped (server restart, network blip, idle timeout).
   if(g_sock == INVALID_HANDLE || !SocketIsConnected(g_sock))
     {
      g_status = 1;
      if(!Connect())
        {
         PanelUpdate();
         return;   // retry on the next timer tick
        }
     }
   datetime now = TimeCurrent();
   if(now - g_last_hb >= (datetime)InpHeartbeatSecs)
     {
      SendLine("{\"t\":\"heartbeat\",\"v\":1,\"time\":" + IntegerToString((long)now) + "}\n");
      g_last_hb = now;
     }
   string line;
   while(RecvLine(line))
      if(StringFind(line, "\"t\":\"order\"") >= 0)
        {
         g_orders++;
         SendLine(ExecuteOrder(line));
        }
   PanelUpdate();
  }
//+------------------------------------------------------------------+
