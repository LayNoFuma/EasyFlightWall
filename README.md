# EasyFlightWall
I've created this cuz my gf asked me to.

This project is a DIY Flight Display Panel, inspired by the original FlightWall concept created by aarenstade and _alex_cronin.

<img width="384" height="512" alt="1" src="https://github.com/user-attachments/assets/5c9a85e2-3e01-46bd-8801-3c68057ba45d" />
<img width="384" height="512" alt="2" src="https://github.com/user-attachments/assets/dadf5069-a90c-4a62-b381-6780b40c1f75" />


The goal is to recreate a compact version of a real airport-style display using:

ESP32 microcontroller: https://it.aliexpress.com/item/1005005704190069.html?spm=a2g0o.order_list.order_list_main.23.43903696J9OIz8&gatewayAdapt=glo2ita

HUB75 LED panel: https://it.aliexpress.com/item/1005007439017560.html?spm=a2g0o.order_list.order_list_main.5.43903696J9OIz8&gatewayAdapt=glo2ita

real-time flight data from: OpenSky Network (live aircraft positions) and FlightAware AeroAPI (flight details)

In addition I used mortaca DMDos board as an adapter between the ESP and the panel wich you can buy here https://www.mortaca.com/

The result is a live, always-on flight tracker display showing nearby aircraft with:

Callsign
Origin → Destination
Aircraft type
Animated UI elements
Standby clock & sleep mode

# Assemble
Connect the ESP32 using mortaca adapter to the panel, then, connect the wires for 5v+ and GND to the adapter
<img width="533" height="405" alt="4" src="https://github.com/user-attachments/assets/c42de45e-a191-477d-8c31-4133e470d767" />

# API Configuration
OpenSky (Required)

Create an account
Create an API client
Get: client_id and client_secret

FlightAware AeroAPI (Required)

Create an account
Generate an API key

# Configure credentials
Edit main.cpp:
```
const char* WIFI_SSID     = "YOUR_WIFI_SSID";

const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* FLIGHTAWARE_API_KEY = "YOUR_FLIGHTAWARE_API_KEY";

const char* OPENSKY_CLIENT_ID     = "YOUR_OPENSKY_CLIENT_ID";

const char* OPENSKY_CLIENT_SECRET = "YOUR_OPENSKY_CLIENT_SECRET";

```

# Configure position

Edit this line:
```
"https://opensky-network.org/api/states/all?lamin=X&lomin=Y&lamax=X2&lomax=Y2"
```

# How it works
Step 1 – OpenSky
Fetches live aircraft data in a geographic area
Selects the closest aircraft

Step 2 – FlightAware
Queries additional flight details using the callsign
Retrieves:

origin
destination
aircraft type

# API Usage Strategy
To avoid rate limits:

OpenSky:
~20 seconds interval
OAuth2 authentication

FlightAware:
Called ONLY when callsign changes

This ensures:
stable performance
minimal API usage
no HTTP 429 errors
