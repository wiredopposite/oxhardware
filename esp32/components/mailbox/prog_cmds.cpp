#include "host_link.hpp"
#include "prog_server.hpp"

namespace ProgServer {
namespace {
Parser In;
Packet Out;

void Xfer(HostLink::Args &args, HostLink::Reply &reply)
{
    const size_t n = args.Remaining();
    In.Reset();
    In.Feed(args.Bytes(n), n);
    if (!In.Complete()) {
        reply.Fail(HostLink::Error::BadArgs, "not a complete packet");
        return;
    }
    Handle(In, Out);
    reply.Put(Out.Data, Out.Size);
}

}

void RegisterCommands()
{
    HostLink::Register(HostLink::Cmd::ProgXfer, Xfer);
}

}
