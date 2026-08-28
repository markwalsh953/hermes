/*
 * secrets.example.h - template. Copy this file to secrets.h and fill in your values.
 * secrets.h is gitignored so your real APN / keys never get published.
 */
#pragma once

// ---- Cellular ----
// APN for your SIM's network - check with your SIM/network provider. Leave
// empty ("") if your provider's network assigns one automatically.
#define CELLULAR_APN "your.apn.here"

// ---- Firebase Realtime Database ----
// Host only: no "https://", no trailing slash. Example:
//   your-project-default-rtdb.europe-west1.firebasedatabase.app
#define FIREBASE_HOST "YOUR_PROJECT-default-rtdb.REGION.firebasedatabase.app"

// Web API key from Firebase console -> Project settings -> General ->
// "Web API Key". Used to sign the device in anonymously so writes carry a
// real auth token (see firebase/database.rules.json and the README's
// "Firebase security" section). Enable the "Anonymous" sign-in provider
// under Authentication -> Sign-in method first.
#define FIREBASE_API_KEY "YOUR_FIREBASE_WEB_API_KEY"
