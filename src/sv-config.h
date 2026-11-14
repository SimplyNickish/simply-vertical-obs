// Where the plugin talks to. The keys are the public anon keys, the same ones every web page ships.
// A normal build talks to the live site. Only a build made with -DSV_DEV_BUILD=ON talks to the
// Dev database and the local site, so a public download can never point at Dev by mistake.
#pragma once
#ifdef SV_DEV_BUILD
#define SV_SUPABASE_URL "https://tqaelzrjwnbtijuczxze.supabase.co"
#define SV_SUPABASE_ANON_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InRxYWVsenJqd25idGlqdWN6eHplIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODYyMzM3OTEsImV4cCI6MjEwMTgwOTc5MX0.yYjqBnHECEz4PaeMi-IV_R3wkko4jO1R0BL1gEVZFpU"
#define SV_SITE_URL "http://localhost:3009"
#else
#define SV_SUPABASE_URL "https://unwmpaaqdimawyqlqhrz.supabase.co"
#define SV_SUPABASE_ANON_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InVud21wYWFxZGltYXd5cWxxaHJ6Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzY1NjU2NzEsImV4cCI6MjA5MjE0MTY3MX0.EE31NUhkxQzkOrxTDZOOnW7a705eOrj5I086CXIEucM"
#define SV_SITE_URL "https://vertical.simplynickish.com"
#endif
