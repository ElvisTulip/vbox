
################################################################################
#  Socket Applications
################################################################################

[Components]
  AppPkg/Applications/Sockets/DataSink/DataSink.inf
  AppPkg/Applications/Sockets/DataSource/DataSource.inf
  AppPkg/Applications/Sockets/GetHostByAddr/GetHostByAddr.inf
  AppPkg/Applications/Sockets/GetHostByDns/GetHostByDns.inf
  AppPkg/Applications/Sockets/GetHostByName/GetHostByName.inf
  AppPkg/Applications/Sockets/GetNetByAddr/GetNetByAddr.inf
  AppPkg/Applications/Sockets/GetNetByName/GetNetByName.inf
  AppPkg/Applications/Sockets/GetServByName/GetServByName.inf
  AppPkg/Applications/Sockets/GetServByPort/GetServByPort.inf
  AppPkg/Applications/Sockets/OobRx/OobRx.inf
  AppPkg/Applications/Sockets/OobTx/OobTx.inf
  AppPkg/Applications/Sockets/RawIp4Rx/RawIp4Rx.inf
  AppPkg/Applications/Sockets/RawIp4Tx/RawIp4Tx.inf
  AppPkg/Applications/Sockets/RecvDgram/RecvDgram.inf
  AppPkg/Applications/Sockets/SetHostName/SetHostName.inf
  AppPkg/Applications/Sockets/SetSockOpt/SetSockOpt.inf
  AppPkg/Applications/Sockets/TftpServer/TftpServer.inf
  AppPkg/Applications/Sockets/WebServer/WebServer.inf {
    <PcdsFixedAtBuild>
      gStdLibTokenSpaceGuid.WebServer_HttpPort|80
  }

