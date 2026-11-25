import React, {useEffect, useState} from 'react';
import {ScrollView, StyleSheet, Text, View} from 'react-native';
import {ProbeNative} from '@amazon-devices/probenative';
import {language, locale} from '../i18n';
import {APP_BUILD} from '../rules';

type Status = {
  running: boolean;
  ip: string;
  iface: string;
  name: string;
  ports: Record<string, string>;
  tcpAccepted: number;
  peers: string[];
  mdns: {
    state: string;
    queriesSeen: number;
    answered: number;
    airplayQueries: number;
  };
  ssdp: {state: string; searchesSeen: number; answered: number};
  video: {received: number; buffered: number; dropped: number};
  airplay?: {
    state: string;
    video: {
      frames: number;
      muxed: number;
      inits: number;
      keyframes: number;
      keyGapSec: number;
      bytes: number;
      width: number;
      height: number;
      h265: boolean;
    };
    audio: {
      codec: string;
      packets: number;
      decodeErrors: number;
      seconds: number;
      flacBytes: number;
      decodeMs: number;
      encodeMs: number;
      cpuPct: number;
    };
  };
  log: string[];
};

/**
 * The feasibility probe's status screen, over the app in debug builds (Menu key on the remote).
 * The same data is at GET http://<tv>:7000/status.
 */
export const DebugPanel = () => {
  const [status, setStatus] = useState<Status | null>(null);
  useEffect(() => {
    const read = () => {
      try {
        setStatus(JSON.parse(ProbeNative.status()));
      } catch {
        // ignore
      }
    };
    read();
    const timer = setInterval(read, 1000);
    return () => clearInterval(timer);
  }, []);

  const ip = status?.ip || '?';
  return (
    <View style={styles.root}>
      <View style={styles.left}>
        <Text style={styles.title}>Screenatic debug · Menu closes</Text>
        <Text style={styles.line}>
          locale {locale} → {language} · build {APP_BUILD}
        </Text>
        <Text style={styles.big}>
          {status?.running ? 'Running' : 'Not running'} · {ip} (
          {status?.iface || '-'})
        </Text>
        <Section title="Ports">
          {Object.entries(status?.ports ?? {}).map(([k, v]) => (
            <Line key={k} text={`${k}: ${v}`} />
          ))}
          <Line
            text={`TCP connections accepted: ${status?.tcpAccepted ?? 0}`}
          />
        </Section>
        <Section title="mDNS (AirPlay discovery)">
          <Line text={status?.mdns.state ?? '-'} />
          <Line
            text={`queries seen ${
              status?.mdns.queriesSeen ?? 0
            } · AirPlay queries ${
              status?.mdns.airplayQueries ?? 0
            } · answered ${status?.mdns.answered ?? 0}`}
          />
        </Section>
        <Section title="SSDP (DLNA discovery)">
          <Line text={status?.ssdp.state ?? '-'} />
          <Line
            text={`searches seen ${status?.ssdp.searchesSeen ?? 0} · answered ${
              status?.ssdp.answered ?? 0
            }`}
          />
        </Section>
        <Section title="AirPlay core (UxPlay)">
          <Line text={status?.airplay?.state ?? 'not started'} />
          <Line
            text={`video: ${status?.airplay?.video.frames ?? 0} frames (${
              status?.airplay?.video.muxed ?? 0
            } to MSE, ${status?.airplay?.video.inits ?? 0} init, ${
              status?.airplay?.video.keyframes ?? 0
            } keyframes, last gap ${(
              status?.airplay?.video.keyGapSec ?? -1
            ).toFixed(1)} s), ${Math.round(
              (status?.airplay?.video.bytes ?? 0) / 1024,
            )} KiB, ${status?.airplay?.video.width ?? 0}x${
              status?.airplay?.video.height ?? 0
            }`}
          />
          <Line text={`audio: ${status?.airplay?.audio.codec ?? '-'}`} />
          <Line
            text={`audio: ${(status?.airplay?.audio.seconds ?? 0).toFixed(
              1,
            )} s decoded, ${
              status?.airplay?.audio.decodeErrors ?? 0
            } errors, CPU ${(status?.airplay?.audio.cpuPct ?? 0).toFixed(
              1,
            )}% (decode ${status?.airplay?.audio.decodeMs ?? 0} ms + FLAC ${
              status?.airplay?.audio.encodeMs ?? 0
            } ms)`}
          />
        </Section>
        <Section title="Video ingest (:7100)">
          <Line
            text={`received ${Math.round(
              (status?.video.received ?? 0) / 1024,
            )} KiB · buffered ${Math.round(
              (status?.video.buffered ?? 0) / 1024,
            )} KiB`}
          />
        </Section>
        <Section title="Remote peers">
          {(status?.peers ?? []).map((p) => (
            <Line key={p} text={p} />
          ))}
        </Section>
        <Section title="How to test">
          <Line
            text={`1. From the Mac: curl http://${ip}:7000/  and  curl http://${ip}:49595/description.xml`}
          />
          <Line text="2. iPhone: Control Center → Screen Mirroring → is “Screenatic Probe” listed?" />
          <Line text="3. Mac: dns-sd -B _airplay._tcp   (should list Screenatic Probe)" />
          <Line text="4. Windows: Win+K / Cast to device, or a DLNA app on Android" />
          <Line text={`5. Video latency: vega/scripts/stream_test.sh ${ip}`} />
        </Section>
      </View>
      <View style={styles.right}>
        <Text style={styles.section}>Log</Text>
        <ScrollView>
          {(status?.log ?? []).map((l, i) => (
            <Text key={i} style={styles.log}>
              {l}
            </Text>
          ))}
        </ScrollView>
      </View>
    </View>
  );
};

const Section = ({
  title,
  children,
}: {
  title: string;
  children: React.ReactNode;
}) => (
  <View style={styles.block}>
    <Text style={styles.section}>{title}</Text>
    {children}
  </View>
);

const Line = ({text}: {text: string}) => (
  <Text style={styles.line}>{text}</Text>
);

const styles = StyleSheet.create({
  root: {
    ...StyleSheet.absoluteFillObject,
    flexDirection: 'row',
    backgroundColor: 'rgba(16,20,24,0.92)',
    padding: 20,
  },
  left: {flex: 1, paddingRight: 12},
  right: {flex: 1, backgroundColor: '#182028', padding: 10, borderRadius: 6},
  title: {color: '#fff', fontSize: 18, fontWeight: '600'},
  big: {color: '#9fe39f', fontSize: 15, marginTop: 2, marginBottom: 4},
  block: {marginTop: 6},
  section: {color: '#7fb2ff', fontSize: 13, fontWeight: '600', marginBottom: 1},
  line: {color: '#e8e8e8', fontSize: 11},
  log: {color: '#c8c8c8', fontSize: 10, fontFamily: 'monospace'},
});
