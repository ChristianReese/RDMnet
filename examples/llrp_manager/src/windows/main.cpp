/******************************************************************************
************************* IMPORTANT NOTE -- READ ME!!! ************************
*******************************************************************************
* THIS SOFTWARE IMPLEMENTS A **DRAFT** STANDARD, BSR E1.33 REV. 77. UNDER NO
* CIRCUMSTANCES SHOULD THIS SOFTWARE BE USED FOR ANY PRODUCT AVAILABLE FOR
* GENERAL SALE TO THE PUBLIC. DUE TO THE INEVITABLE CHANGE OF DRAFT PROTOCOL
* VALUES AND BEHAVIORAL REQUIREMENTS, PRODUCTS USING THIS SOFTWARE WILL **NOT**
* BE INTEROPERABLE WITH PRODUCTS IMPLEMENTING THE FINAL RATIFIED STANDARD.
*******************************************************************************
* Copyright 2019 ETC Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************
* This file is a part of RDMnet. For more information, go to:
* https://github.com/ETCLabs/RDMnet
******************************************************************************/

#include <cwchar>
#include <iostream>

#include <WinSock2.h>
#include <Windows.h>
#include <WS2tcpip.h>

#include "lwpa/uuid.h"
#include "manager.h"

std::string ConsoleInputToUtf8(const std::wstring &input)
{
  if (!input.empty())
  {
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, NULL, 0, NULL, NULL);
    if (size_needed > 0)
    {
      std::string str_res(size_needed, '\0');
      int convert_res = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, &str_res[0], size_needed, NULL, NULL);
      if (convert_res > 0)
      {
        return str_res;
      }
    }
  }

  return std::string();
}

int wmain(int /*argc*/, wchar_t * /*argv*/ [])
{
  LwpaUuid manager_cid;

  UUID uuid;
  UuidCreate(&uuid);
  memcpy(manager_cid.data, &uuid, LWPA_UUID_BYTES);

  LLRPManager mgr(manager_cid);
  printf("Discovered network interfaces:\n");
  mgr.PrintNetints();
  mgr.PrintCommandList();

  std::wstring input;
  do
  {
    std::getline(std::wcin, input);
  } while (mgr.ParseCommand(ConsoleInputToUtf8(input)));

  return 0;
}
