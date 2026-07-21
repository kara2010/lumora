// Herstellerspezifische Portfreigabe-Anleitung, wenn die automatische UPnP-Freigabe
// scheitert. 1:1 aus main.js routerUpnpHint portiert. Texte pro Router als volle
// nummerierte Schritt-fuer-Schritt-Anleitung (DE/EN), Menuepfade an offiziellen
// Quellen verifiziert. Der Renderer zeigt "steps" 1:1 an - KEIN i18n-Lookup.
// {T}/{U} werden durch den TCP- bzw. UDP-Port ersetzt.
#pragma once
#include <string>
#include <vector>
#include <regex>

namespace lurouter {

struct RouterHint { std::string router; std::string steps; };

inline std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) { s.replace(p, from.size(), to); p += to.size(); }
    return s;
}

// name = friendlyName/Modell des erkannten Routers (kann leer sein). de = true -> deutsche Anleitung.
inline RouterHint routerUpnpHint(const std::string& name, bool de, int tcpPort, int udpPort) {
    std::string nm = name;
    for (auto& c : nm) c = (char)tolower((unsigned char)c);

    struct Brand { const char* re; const char* brand; const char* de; const char* en; };
    static const Brand brands[] = {
      { R"(avm|fritz|1 ?und ?1|1 ?& ?1|homeserver)", "FRITZ!Box",
R"TXT(Öffne fritz.box im Browser und melde dich an.
1. „Internet › Freigaben › Portfreigaben“ öffnen.
2. „Gerät für Freigaben hinzufügen“ klicken und im Feld „Gerät“ diesen PC wählen. Fehlt er, wähle „IP-Adresse manuell eingeben“ und trage seine IP ein.
3. „Neue Freigabe“ › „Portfreigabe“ wählen; bei „Anwendung“ den Punkt „Andere Anwendung“.
4. Bezeichnung „Lumora TCP“, Protokoll TCP, in allen Portfeldern {T}. Mit „OK“ speichern.
5. Noch einmal „Neue Freigabe“: „Lumora UDP“, Protokoll UDP, Port {U}. Speichern.
6. Unten mit „Übernehmen“ bestätigen.
Tipp: Hakst du beim PC-Eintrag „Selbstständige Portfreigaben für dieses Gerät erlauben“ an, öffnet Lumora den Port künftig selbst.)TXT",
R"TXT(Open fritz.box in your browser and sign in.
1. Go to “Internet › Permit Access › Port Sharing”.
2. Click “Add Device for Sharing” and pick this PC in the “Device” field. If it's missing, choose “Enter IP address manually” and type its IP.
3. “New Port Sharing” › “Port Sharing”; under “Application” choose “Other application”.
4. Name it “Lumora TCP”, protocol TCP, port {T} in every port field. Save with “OK”.
5. Add another: “Lumora UDP”, protocol UDP, port {U}. Save.
6. Confirm with “Apply” at the bottom.
Tip: If you tick “Allow independent port sharing for this device” on the PC entry, Lumora can open the port itself next time.)TXT" },

      { R"(speedport|telekom)", "Speedport",
R"TXT(Öffne speedport.ip (oder 192.168.2.1) und melde dich mit dem Gerätepasswort (Router-Rückseite) an. Speedport-Router können KEIN UPnP – die manuelle Freigabe ist hier der einzige Weg.
1. Gib dem PC zuerst eine feste IP: „Netzwerk › Verbundene Geräte“ › diesen PC über „zeigen“ öffnen › Haken „Immer dieselbe IPv4-Adresse“ › Speichern.
2. „Internet › Portfreischaltung“ öffnen.
3. Einen Namen eintragen (z. B. „Lumora Stream“).
4. In der Liste diesen PC (das Gerät) auswählen.
5. Ohne Vorlage den Port {T} eintragen (öffentlich = intern) und speichern.
6. Über „Weitere Port-Weiterleitung“ eine zweite Regel mit Port {U} anlegen. Speichern.)TXT",
R"TXT(Open speedport.ip (or 192.168.2.1) and sign in with the device password (on the back of the router). Speedport routers do NOT support UPnP – the manual steps are the only way here.
1. First give the PC a fixed IP: “Network › Connected devices” › open this PC via “show” › tick “Always the same IPv4 address” › Save.
2. Open “Internet › Port forwarding” (“Portfreischaltung”).
3. Enter a name (e.g. “Lumora Stream”).
4. Select this PC (the device) from the list.
5. Without a template, enter port {T} (public = internal) and save.
6. Use “Add further port forwarding” to create a second rule with port {U}. Save.)TXT" },

      { R"(vodafone)", "Vodafone Station",
R"TXT(Öffne 192.168.0.1, klicke oben rechts auf das Menü-Symbol und melde dich an (Passwort auf dem Aufkleber).
1. „Internet › Port-Mapping“ öffnen.
2. Auf das „Hinzufügen“-Symbol klicken.
3. Dienstname „Lumora TCP“, Gerät = dieser PC, LAN-IP = seine Heimnetz-IP, Protokoll TCP, Art „Port aktivieren“, WAN-Port {T}, LAN-Port {T}.
4. „Speichern“ › „Anwenden“ › den Slider nach rechts zum Aktivieren.
5. Zweite Regel genauso, aber Protokoll UDP und Port {U}.
Hinweis: Fehlt der Menüpunkt „Port-Mapping“, hast du einen DS-Lite-Anschluss (keine öffentliche IPv4) – dann ist keine Freigabe möglich; nutze den IPv6-Weg (unten) oder beantrage bei Vodafone „Dual Stack“.)TXT",
R"TXT(Open 192.168.0.1, click the menu icon at the top right and sign in (password on the sticker).
1. Open “Internet › Port-Mapping”.
2. Click the “Add” icon.
3. Service name “Lumora TCP”, Device = this PC, LAN IP = its home-network IP, Protocol TCP, Type “Enable port”, WAN port {T}, LAN port {T}.
4. “Save” › “Apply” › drag the slider to the right to activate.
5. Add a second rule the same way, but Protocol UDP and port {U}.
Note: If there's no “Port-Mapping” menu, you have a DS-Lite line (no public IPv4) – then forwarding is impossible; use the IPv6 path (below) or ask Vodafone for “Dual Stack”.)TXT" },

      { R"(\bo2\b|telefonica|homebox|o2 ?box)", "o2 HomeBox",
R"TXT(Öffne o2.box (oder 192.168.1.1) und melde dich an (Passwort auf der Geräterückseite).
1. Gib dem PC eine feste IP (DHCP-Reservierung im Router).
2. Menüpunkt „IPv4/IPv6-Portfreigabe“ öffnen (unterstützt TCP und UDP).
3. Neue Regel: Dienst frei benennen, Rechner-IP = dieser PC, Port {T}, Protokoll TCP; speichern/aktivieren.
4. Zweite Regel: Port {U}, Protokoll UDP.
Hinweis: Bei einem DS-Lite-Anschluss ist keine IPv4-Freigabe möglich – dann den IPv6-Weg (unten) nutzen. (o2-Kabel liefert oft FRITZ!Box-Modelle – dann gilt die FRITZ!Box-Anleitung.))TXT",
R"TXT(Open o2.box (or 192.168.1.1) and sign in (password on the back of the device).
1. Give the PC a fixed IP (DHCP reservation in the router).
2. Open the “IPv4/IPv6 port forwarding” menu (supports TCP and UDP).
3. New rule: name the service freely, Computer IP = this PC, port {T}, protocol TCP; save/activate.
4. Second rule: port {U}, protocol UDP.
Note: On a DS-Lite line no IPv4 forwarding is possible – use the IPv6 path (below). (o2 cable often ships FRITZ!Box models – then use the FRITZ!Box guide.))TXT" },

      { R"(tp-?link|archer)", "TP-Link",
R"TXT(Öffne tplinkwifi.net (oder 192.168.0.1) im Browser und melde dich an.
1. „Advanced“ (Erweitert) › „NAT Forwarding“ › „Virtual Servers“ öffnen (bei manchen Modellen heißt es „Port Forwarding“).
2. „Add“ klicken: Service Name „Lumora-TCP“, External Port {T}, Internal Port {T}, Protokoll TCP; Internal IP = dieser PC (bequem über „View Connected Devices“ wählen). Speichern.
3. Noch einmal „Add“: „Lumora-UDP“, Port {U}, Protokoll UDP, gleiche PC-IP. Speichern.
Tipp: Gib dem PC vorher eine feste IP (Address Reservation), sonst wandert die Regel bei IP-Wechsel ins Leere. UPnP-Alternative: „Advanced › NAT Forwarding › UPnP“ einschalten – dann macht Lumora es selbst.)TXT",
R"TXT(Open tplinkwifi.net (or 192.168.0.1) in your browser and sign in.
1. Open “Advanced” › “NAT Forwarding” › “Virtual Servers” (on some models it's called “Port Forwarding”).
2. Click “Add”: Service Name “Lumora-TCP”, External Port {T}, Internal Port {T}, Protocol TCP; Internal IP = this PC (pick it via “View Connected Devices”). Save.
3. Click “Add” again: “Lumora-UDP”, port {U}, protocol UDP, same PC IP. Save.
Tip: Give the PC a fixed IP first (Address Reservation), otherwise the rule breaks when the IP changes. UPnP alternative: enable “Advanced › NAT Forwarding › UPnP” – then Lumora does it itself.)TXT" },

      { R"(asus(tek)?)", "ASUS",
R"TXT(Öffne router.asus.com (oder 192.168.1.1) im Browser und melde dich an.
1. Links „WAN“ › Reiter „Virtual Server / Port Forwarding“.
2. „Enable Port Forwarding“ auf ON stellen.
3. „Add profile“: Service Name „Lumora-TCP“, Protocol TCP, External Port {T}, Internal Port {T}, Internal IP = deine PC-IP. Übernehmen.
4. Zweites „Add profile“: Protocol UDP, Port {U}, gleiche PC-IP. Mit „Apply“ speichern.
Tipp: Gib dem PC vorher per DHCP-Reservierung eine feste IP. UPnP-Alternative: „WAN › Internet Connection › Enable UPnP: Yes“.)TXT",
R"TXT(Open router.asus.com (or 192.168.1.1) in your browser and sign in.
1. On the left open “WAN” › tab “Virtual Server / Port Forwarding”.
2. Set “Enable Port Forwarding” to ON.
3. “Add profile”: Service Name “Lumora-TCP”, Protocol TCP, External Port {T}, Internal Port {T}, Internal IP = your PC's IP. Apply.
4. Second “Add profile”: Protocol UDP, port {U}, same PC IP. Save with “Apply”.
Tip: Give the PC a fixed IP via DHCP reservation first. UPnP alternative: “WAN › Internet Connection › Enable UPnP: Yes”.)TXT" },

      { R"(netgear|nighthawk|\borbi\b)", "Netgear",
R"TXT(Öffne routerlogin.net im Browser, Benutzer „admin“ und dein Passwort.
1. „ADVANCED“ › „Advanced Setup“ › „Port Forwarding/Port Triggering“; Punkt „Port Forwarding“ ausgewählt lassen.
2. „Add Custom Service“: Name „Lumora-TCP“, Type TCP, External Start+Ende je {T}, interner Port gleich; Server-IP = dieser PC (unten in der Geräteliste anklickbar). „Apply“.
3. Noch einmal „Add Custom Service“: Type UDP, Port {U}. „Apply“.
Tipp: Gib dem PC vorher eine reservierte IP (Address Reservation). UPnP-Alternative: „ADVANCED › Advanced Setup › UPnP › Turn UPnP On“.)TXT",
R"TXT(Open routerlogin.net in your browser, user “admin” and your password.
1. “ADVANCED” › “Advanced Setup” › “Port Forwarding/Port Triggering”; keep “Port Forwarding” selected.
2. “Add Custom Service”: Name “Lumora-TCP”, Type TCP, External Starting+Ending port {T}, same internal port; Server IP = this PC (selectable in the device list below). “Apply”.
3. “Add Custom Service” again: Type UDP, port {U}. “Apply”.
Tip: Give the PC a reserved IP first (Address Reservation). UPnP alternative: “ADVANCED › Advanced Setup › UPnP › Turn UPnP On”.)TXT" },

      { R"(d-?link)", "D-Link",
R"TXT(Öffne dlinkrouter.local (oder 192.168.0.1) im Browser und melde dich an.
1. Oben auf „Features“ › „Port Forwarding“ gehen.
2. „Add Rule“: Name „Lumora“, Local IP = dieser PC (aus dem Dropdown wählbar).
3. TCP Port {T} und UDP Port {U} in ihre jeweiligen Felder eintragen (bei D-Link decken beide Ports EINE Regel ab).
4. „Schedule“ auf „Always Enable“, dann „Save“.
Tipp: Gib dem PC vorher eine feste IP (DHCP-Reservierung).)TXT",
R"TXT(Open dlinkrouter.local (or 192.168.0.1) in your browser and sign in.
1. At the top go to “Features” › “Port Forwarding”.
2. “Add Rule”: Name “Lumora”, Local IP = this PC (selectable from the dropdown).
3. Enter TCP port {T} and UDP port {U} in their fields (on D-Link both ports fit in ONE rule).
4. Set “Schedule” to “Always Enable”, then “Save”.
Tip: Give the PC a fixed IP first (DHCP reservation).)TXT" },

      { R"(linksys|belkin)", "Linksys",
R"TXT(Öffne LinksysSmartWiFi.com (oder 192.168.1.1); lokal über „click here“ mit dem Router-Passwort anmelden.
1. „Router Settings › Security › Apps and Gaming › Single Port Forwarding“ öffnen.
2. „Add a new Single Port Forwarding“: „Lumora TCP“, External Port {T}, Internal Port {T}, Protocol TCP, Device IP# = dieser PC. „Enabled“ anhaken.
3. Zweite Regel: „Lumora UDP“, Port {U}, Protocol UDP, gleiche PC-IP. „Enabled“. Dann „Save“ › „Apply“ › „Ok“.
Wichtig: Linksys verlangt eine feste IP – gib dem PC vorher eine statische/reservierte IP.)TXT",
R"TXT(Open LinksysSmartWiFi.com (or 192.168.1.1); sign in locally via “click here” with the router password.
1. Open “Router Settings › Security › Apps and Gaming › Single Port Forwarding”.
2. “Add a new Single Port Forwarding”: “Lumora TCP”, External Port {T}, Internal Port {T}, Protocol TCP, Device IP# = this PC. Tick “Enabled”.
3. Second rule: “Lumora UDP”, port {U}, Protocol UDP, same PC IP. Tick “Enabled”. Then “Save” › “Apply” › “Ok”.
Important: Linksys requires a fixed IP – give the PC a static/reserved IP first.)TXT" },

      { R"(huawei)", "Huawei",
R"TXT(Öffne 192.168.3.1 im Browser und melde dich an (Passwort auf dem Typenschild).
1. „More Functions › Security Settings › NAT Services“ öffnen.
2. Bei „Port Forwarding“ auf das Plus/„Hinzufügen“-Symbol klicken.
3. Service name „Lumora TCP“, Device name = dieser PC (aus der Geräteliste – der PC muss verbunden sein), Protocol TCP, interner + externer Port {T}. „Save“.
4. Zweite Regel: „Lumora UDP“, Protocol UDP, Port {U}. „Save“.
Hinweis: UPnP ist ab Werk AUS. Möchtest du, dass Lumora es selbst macht, aktiviere „More Functions › Network Settings › UPnP“.)TXT",
R"TXT(Open 192.168.3.1 in your browser and sign in (password on the label).
1. Open “More Functions › Security Settings › NAT Services”.
2. Click the plus/“Add” icon next to “Port Forwarding”.
3. Service name “Lumora TCP”, Device name = this PC (from the device list – the PC must be connected), Protocol TCP, internal + external port {T}. “Save”.
4. Second rule: “Lumora UDP”, Protocol UDP, port {U}. “Save”.
Note: UPnP is off by default. To let Lumora do it itself, enable “More Functions › Network Settings › UPnP”.)TXT" },

      { R"(bt\s?smart\s?hub|bthomehub|\bbt\s?hub\b)", "BT Smart Hub",
R"TXT(Öffne 192.168.1.254 im Browser (Admin-Passwort auf der Hub-Karte / Rückseite).
1. „Advanced Settings“ › „Firewall“ öffnen, Passwort eingeben.
2. „Create a new port forwarding rule“ klicken, Namen vergeben (z. B. „Lumora-TCP“).
3. Unter „Select the device …“ diesen PC aus der Geräteliste wählen (BT bindet ans Gerät, keine IP tippen).
4. External + Internal Port je {T}, Protocol TCP › „Set“ › „Save“.
5. Zweite Regel identisch für Port {U}, Protocol UDP.
Tipp: Zur Sicherheit die IP reservieren unter „Advanced settings › My Network“ › Gerät › „Always use this IP address = YES“.)TXT",
R"TXT(Open 192.168.1.254 in your browser (admin password on the hub card / back).
1. Open “Advanced Settings” › “Firewall”, enter the password.
2. Click “Create a new port forwarding rule”, give it a name (e.g. “Lumora-TCP”).
3. Under “Select the device …” pick this PC from the device list (BT binds to the device, no IP typing).
4. External + Internal port {T} each, Protocol TCP › “Set” › “Save”.
5. Add an identical second rule for port {U}, Protocol UDP.
Tip: To be safe, reserve the IP under “Advanced settings › My Network” › device › “Always use this IP address = YES”.)TXT" },

      { R"(sky\s?hub|sky\s?router|sky\s?broadband)", "Sky Router",
R"TXT(Sky hat zwei Hub-Typen – der Weg unterscheidet sich:
SCHWARZE Hubs (Sky Hub / Q Hub / Broadband Hub): Öffne 192.168.0.1 › Reiter „Advanced“, anmelden.
1. „Advanced › LAN IP Setup › Address Reservation › Add“: diesen PC wählen, die angezeigte IP notieren, „Add“.
2. „Security › Services › Add Service“: Name „Lumora-TCP“, Type „TCP/UDP“, Start+Finish Port {T} › „Apply“. Zweiten Service „Lumora-UDP“ mit Port {U}/UDP anlegen.
3. „Security › Firewall Rules“ › unter „Inbound Services“ auf „Add“: Service wählen, „Action = ALLOW always“, bei „Destination LAN address“ die notierte PC-IP › „Apply“.
WEISSER WiFi-Max-Hub (SR213): geht nur in der „My Sky“-App › Broadband › Product Settings › Advanced settings › Gerät wählen › Ports {T} (TCP) und {U} (UDP) eintragen › Save.)TXT",
R"TXT(Sky has two hub types – the path differs:
BLACK hubs (Sky Hub / Q Hub / Broadband Hub): Open 192.168.0.1 › “Advanced” tab, sign in.
1. “Advanced › LAN IP Setup › Address Reservation › Add”: select this PC, note the IP shown, “Add”.
2. “Security › Services › Add Service”: Name “Lumora-TCP”, Type “TCP/UDP”, Start+Finish port {T} › “Apply”. Add a second service “Lumora-UDP” with port {U}/UDP.
3. “Security › Firewall Rules” › under “Inbound Services” click “Add”: pick the service, “Action = ALLOW always”, set “Destination LAN address” to the noted PC IP › “Apply”.
WHITE WiFi Max hub (SR213): only via the “My Sky” app › Broadband › Product Settings › Advanced settings › select the device › enter ports {T} (TCP) and {U} (UDP) › Save.)TXT" },

      { R"(virgin\s?media|super\s?hub)", "Virgin Media Hub",
R"TXT(Wichtig: Der Hub muss im „Router mode“ laufen – im „Modem mode“ ist Portfreigabe abgeschaltet.
Öffne 192.168.0.1 (Settings-Passwort auf der Unterseite des Hubs, nicht das WLAN-Passwort).
1. „Advanced settings › DHCP“: diesen PC in der Liste finden und seine IP notieren (idealerweise fest zuordnen).
2. „Advanced settings › Security › Port Forwarding“ öffnen.
3. Regel 1: Local + External Start/End Port je {T}, Protocol TCP, Ziel = die notierte PC-IP.
4. Regel 2: Ports je {U}, Protocol UDP.
5. Speichern (der Hub verlangt evtl. einen Neustart).)TXT",
R"TXT(Important: The hub must run in “Router mode” – in “Modem mode” port forwarding is disabled.
Open 192.168.0.1 (settings password on the base of the hub, not the Wi-Fi password).
1. “Advanced settings › DHCP”: find this PC in the list and note its IP (ideally reserve it).
2. Open “Advanced settings › Security › Port Forwarding”.
3. Rule 1: Local + External start/end port {T} each, Protocol TCP, target = the noted PC IP.
4. Rule 2: ports {U} each, Protocol UDP.
5. Save (the hub may need a reboot).)TXT" },

      { R"(xfinity|xfi\s?gateway|comcast)", "Xfinity Gateway",
R"TXT(Bei Xfinity xFi geht Portfreigabe NUR über die Xfinity-App (nicht im Web-Menü).
1. Xfinity-App öffnen und anmelden.
2. „WiFi › View WiFi equipment › Advanced Settings › Port Forwarding“.
3. „Add Port Forward“ › „Continue“, dann diesen PC wählen. (Es erscheinen nur Geräte mit DHCP + IPv4 – schalte am PC ggf. die MAC-/WLAN-Zufallsadresse aus.)
4. „Manual Setup“: Port {T}, Protokoll TCP › „Next“.
5. Noch einmal „Add Port Forward › Manual Setup“: Port {U}, Protokoll UDP.
Falls es trotz Regel nicht klappt: in der App unter „Advanced Security“ für diesen PC „Allow Access“ setzen.)TXT",
R"TXT(On Xfinity xFi, port forwarding works ONLY via the Xfinity app (not the web menu).
1. Open the Xfinity app and sign in.
2. “WiFi › View WiFi equipment › Advanced Settings › Port Forwarding”.
3. “Add Port Forward” › “Continue”, then select this PC. (Only devices with DHCP + IPv4 appear – turn off the PC's random/private MAC address if needed.)
4. “Manual Setup”: port {T}, protocol TCP › “Next”.
5. “Add Port Forward › Manual Setup” again: port {U}, protocol UDP.
If it still won't work despite the rule: in the app under “Advanced Security”, set “Allow Access” for this PC.)TXT" },

      { R"(\bbgw\d|at&t|arris.?bgw)", "AT&T Gateway",
R"TXT(AT&T-Gateways bieten kein UPnP – die manuelle Freigabe ist hier Pflicht.
Öffne 192.168.1.254, gib den „Device Access Code“ ein (seitlich/unten am Gerät, Groß-/Kleinschreibung beachten).
1. „Firewall › NAT/Gaming“ öffnen (bei Warnung: Gateway neu starten und fortfahren).
2. „Custom Services“: Service Name „Lumora-TCP“, Global Port Range {T} in beide Felder, Base Host Port {T}, Protocol TCP › „Add“.
3. Zweiten Custom Service „Lumora-UDP“ mit Port {U}, Protocol UDP anlegen.
4. „Return to NAT/Gaming“ › bei „Needed by Device“ diesen PC wählen › „Add“ › „Save“.)TXT",
R"TXT(AT&T gateways don't offer UPnP – the manual steps are mandatory here.
Open 192.168.1.254, enter the “Device Access Code” (on the side/bottom of the device, case-sensitive).
1. Open “Firewall › NAT/Gaming” (if warned: restart the gateway and continue).
2. “Custom Services”: Service Name “Lumora-TCP”, Global Port Range {T} in both fields, Base Host Port {T}, Protocol TCP › “Add”.
3. Add a second custom service “Lumora-UDP” with port {U}, Protocol UDP.
4. “Return to NAT/Gaming” › under “Needed by Device” select this PC › “Add” › “Save”.)TXT" },
    };

    for (auto& b : brands) {
        if (std::regex_search(nm, std::regex(b.re, std::regex::icase))) {
            std::string steps = replaceAll(replaceAll(de ? b.de : b.en, "{T}", std::to_string(tcpPort)), "{U}", std::to_string(udpPort));
            return { name.empty() ? std::string(b.brand) : name, steps };
        }
    }

    // Marke unklar: allgemeingueltiges Muster; bekannter Provider-Chipsatz -> neutraler Hinweis.
    const char* genericDe =
R"TXT(Öffne den Router-Adminbereich im Browser (meist 192.168.0.1 oder 192.168.1.1; das Passwort steht oft auf dem Router). Anbieter-Router lassen sich manchmal nur über die App/das Kundenportal des Anbieters einstellen.
1. Gib diesem PC eine feste IP (im Router unter „DHCP“ / „Adressreservierung“), damit die Freigabe stabil bleibt.
2. Suche den Bereich „Portfreigabe“, „Port Forwarding“, „Virtual Server“ oder „NAT“.
3. Neue Regel: diesen PC (Gerät oder seine IP) auswählen, Port {T}, Protokoll TCP, speichern.
4. Zweite Regel: Port {U}, Protokoll UDP, speichern.
Alternativ „UPnP“ / „selbstständige Portfreigaben“ im Router aktivieren – dann öffnet Lumora den Port selbst.)TXT";
    const char* genericEn =
R"TXT(Open the router admin page in your browser (usually 192.168.0.1 or 192.168.1.1; the password is often printed on the router). Provider routers can sometimes only be changed via the provider's app or customer portal.
1. Give this PC a fixed IP (in the router under “DHCP” / “Address Reservation”) so the rule stays valid.
2. Find the section “Port Forwarding”, “Virtual Server” or “NAT”.
3. New rule: select this PC (device or its IP), port {T}, protocol TCP, save.
4. Second rule: port {U}, protocol UDP, save.
Alternatively enable “UPnP” / “automatic port forwarding” in the router – then Lumora opens the port itself.)TXT";
    std::string steps = replaceAll(replaceAll(de ? genericDe : genericEn, "{T}", std::to_string(tcpPort)), "{U}", std::to_string(udpPort));
    if (std::regex_search(nm, std::regex(R"(arris|technicolor|hitron|sagemcom|compal|askey|zyxel|\bzte\b)", std::regex::icase)))
        return { name.empty() ? (de ? std::string("deinem Anbieter-Router") : std::string("your provider router")) : name, steps };
    return { name.empty() ? (de ? std::string("deinem Router") : std::string("your router")) : name, steps };
}

} // namespace lurouter
