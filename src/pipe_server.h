// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "protocol.h"

#include <functional>
#include <memory>
#include <string>

namespace hikari {

std::wstring currentSessionPipeName();

class PipeServer {
public:
    using Handler = std::function<Json(const Json&)>;
    explicit PipeServer(Handler handler, std::wstring name = currentSessionPipeName());
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;
    void start();
    void stop() noexcept;
    void broadcast(const Json& event);
    static Json requestOnce(const Json& request, unsigned timeoutMs = 2000,
                            const std::wstring& name = currentSessionPipeName());

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hikari
