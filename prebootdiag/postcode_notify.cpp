/*
// Copyright (c) 2026 NVIDIA Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
#include "postcode_notify.hpp"

#include <algorithm>
#include <exception>

namespace prebootdiag
{

std::optional<std::string> postCodePayload(std::string_view name)
{
    if (std::ranges::find(postCodeNames, name) == std::end(postCodeNames))
    {
        return std::nullopt;
    }
    return std::string(R"({"PostCodeName":")") + std::string(name) + R"("})";
}

int notifyPostCode(int argc, const char* const argv[], const NotifyCall& call,
                   std::ostream& err)
{
    if (argc != 2)
    {
        err << "Usage: " << argv[0] << " <PostCodeName>\n\n"
            << "  PostCodeName: one of\n";
        for (auto name : postCodeNames)
        {
            err << "    " << name << "\n";
        }
        return 1;
    }

    auto payload = postCodePayload(argv[1]);
    if (!payload)
    {
        err << "Unknown PostCodeName '" << argv[1] << "'\n";
        return 1;
    }

    try
    {
        call(statePostCodeReceived, *payload);
    }
    catch (const std::exception& e)
    {
        err << "Call failed: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

} // namespace prebootdiag
