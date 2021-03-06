#include <stdlib.h>
#include <windows.h>
#include <Wininet.h>
#include <ras.h>
#include <tchar.h>
#include <stdio.h>
#include "common.h"

void reportWindowsError(const char* action, const char* connName) {
  LPTSTR pErrMsg = NULL;
  DWORD errCode = GetLastError();
  FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|
      FORMAT_MESSAGE_FROM_SYSTEM|
      FORMAT_MESSAGE_ARGUMENT_ARRAY,
      NULL,
      errCode,
      LANG_NEUTRAL,
      pErrMsg,
      0,
      NULL);
  if (NULL != connName) {
    fprintf(stderr, "Error %s for connection '%s': %lu %s\n",
        action, connName, errCode, pErrMsg);
  } else {
    fprintf(stderr, "Error %s: %lu %s\n", action, errCode, pErrMsg);
  }
}

// Stolen from https://github.com/getlantern/winproxy Figure out which Dial-Up
// or VPN connection is active; in a normal LAN connection, this should return
// NULL. NOTE: For some reason this method fails when compiled in Debug mode
// but works every time in Release mode.
// TODO: we may want to find all active connections instead of the first one.
LPTSTR findActiveConnection() {
  DWORD dwCb = sizeof(RASCONN);
  DWORD dwErr = ERROR_SUCCESS;
  DWORD dwRetries = 5;
  DWORD dwConnections = 0;
  RASCONN* lpRasConn = NULL;
  RASCONNSTATUS rasconnstatus;
  rasconnstatus.dwSize = sizeof(RASCONNSTATUS);

  // Loop through in case the information from RAS changes between calls.
  while (dwRetries--) {
    // If the memory is allocated, free it.
    if (NULL != lpRasConn) {
      HeapFree(GetProcessHeap(), 0, lpRasConn);
      lpRasConn = NULL;
    }

    // Allocate the size needed for the RAS structure.
    lpRasConn = (RASCONN*)HeapAlloc(GetProcessHeap(), 0, dwCb);
    if (NULL == lpRasConn) {
      dwErr = ERROR_NOT_ENOUGH_MEMORY;
      break;
    }

    // Set the structure size for version checking purposes.
    lpRasConn->dwSize = sizeof(RASCONN);

    // Call the RAS API then exit the loop if we are successful or an unknown
    // error occurs.
    dwErr = RasEnumConnections(lpRasConn, &dwCb, &dwConnections);
    if (ERROR_INSUFFICIENT_BUFFER != dwErr) {
      break;
    }
  }

  // In the success case, return the first active connection.
  if (ERROR_SUCCESS == dwErr) {
    DWORD i;
    for (i = 0; i < dwConnections; i++) {
      RasGetConnectStatus(lpRasConn[i].hrasconn, &rasconnstatus);
      if (rasconnstatus.rasconnstate == RASCS_Connected){
        return lpRasConn[i].szEntryName;
      }

    }
  }
  return NULL; // Couldn't find an active dial-up/VPN connection; return NULL
}

int initialize(INTERNET_PER_CONN_OPTION_LIST* options) {
  DWORD dwBufferSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);
  options->dwSize = dwBufferSize;
  // NULL for LAN, connection name otherwise.
  options->pszConnection = findActiveConnection();

  options->dwOptionCount = 3;
  options->dwOptionError = 0;
  options->pOptions = (INTERNET_PER_CONN_OPTION*)calloc(3, sizeof(INTERNET_PER_CONN_OPTION));
  if(NULL == options->pOptions) {
    return NO_MEMORY;
  }
  options->pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS;
  options->pOptions[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
  options->pOptions[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
  return RET_NO_ERROR;
}

int query(INTERNET_PER_CONN_OPTION_LIST* options) {
  DWORD dwBufferSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);
  if(!InternetQueryOption(NULL, INTERNET_OPTION_PER_CONNECTION_OPTION, options, &dwBufferSize)) {
    reportWindowsError("querying options", options->pszConnection ? options->pszConnection : "LAN");
    return SYSCALL_FAILED;
  }
  return RET_NO_ERROR;
}

int show()
{
  INTERNET_PER_CONN_OPTION_LIST options;
  int ret = initialize(&options);
  if (ret != RET_NO_ERROR) {
    return ret;
  }
  ret = query(&options);
  if (ret != RET_NO_ERROR) {
    return ret;
  }
  if ((options.pOptions[0].Value.dwValue & PROXY_TYPE_PROXY) > 0) {
    if (options.pOptions[1].Value.pszValue != NULL) {
      printf("%s\n", options.pOptions[1].Value.pszValue);
    }
  }
  return ret;
}

int toggleProxy(bool turnOn, const char* proxyHost, const char* proxyPort)
{
  INTERNET_PER_CONN_OPTION_LIST options;
  int ret = initialize(&options);
  if (ret != RET_NO_ERROR) {
    return ret;
  }

  char *proxy = malloc(256);
  snprintf(proxy, 256, "%s:%s", proxyHost, proxyPort);

  if (turnOn) {
    options.pOptions[0].Value.dwValue = PROXY_TYPE_DIRECT | PROXY_TYPE_PROXY;
    options.pOptions[1].Value.pszValue = proxy;
    options.pOptions[2].Value.pszValue = TEXT("<local>");
  }
  else {
    if (strlen(proxyHost) == 0) {
      goto turnOff;
    }
    ret = query(&options);
    if (ret != RET_NO_ERROR) {
      goto cleanup;
    }
    // we turn proxy off only if the option is set and proxy address has the
    // provided prefix.
    if ((options.pOptions[0].Value.dwValue & PROXY_TYPE_PROXY) == 0
        || options.pOptions[1].Value.pszValue == NULL
        || strncmp(proxy, options.pOptions[1].Value.pszValue, strlen(proxy)) != 0) {
      goto cleanup;
    }
    // fall through
turnOff:
    options.pOptions[0].Value.dwValue = PROXY_TYPE_DIRECT;
    options.pOptions[1].Value.pszValue = "";
    options.pOptions[2].Value.pszValue = "";
  }

  DWORD dwBufferSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);
  BOOL result = InternetSetOption(NULL,
      INTERNET_OPTION_PER_CONNECTION_OPTION,
      &options,
      dwBufferSize);
  if (!result) {
    reportWindowsError("setting options", options.pszConnection ? options.pszConnection : "LAN");
    ret = SYSCALL_FAILED;
    goto cleanup;
  }
  result = InternetSetOption(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
  if (!result) {
    reportWindowsError("propagating changes", NULL);
    ret = SYSCALL_FAILED;
    goto cleanup;
  }
  result = InternetSetOption(NULL, INTERNET_OPTION_REFRESH , NULL, 0);
  if (!result) {
    reportWindowsError("refreshing", NULL);
    ret = SYSCALL_FAILED;
    goto cleanup;
  }

cleanup:
  free(options.pOptions);
  free(proxy);
  return ret;
}
