import React, {useEffect, useState} from 'react';
import {ScrollView, StyleSheet, Text, View} from 'react-native';
import {ProbeNative} from '@amazon-devices/probenative';
import {VideoTest} from './VideoTest';
import {MirrorAudio} from './MirrorAudio';

type Status = {
  running: boolean;
  ip: string;
  iface: string;
  name: string;
  ports: Record<string, string>;
  tcpAccepted: number;
  peers: string[];
  mdns: {state: string; queriesSeen: number; answered: number; airplayQueries: number};
  ssdp: {state: string; searchesSeen: number; answered: number};
  video: {received: number; buffered: number; dropped: number};
  airplay?: {
    state: string;
    video: {frames: number; muxed: number; inits: number; keyframes: number; keyGapSec: number; bytes: number; width: number; height: number; h265: boolean};
    audio: {codec: string; packets: number; decodeErrors: number; seconds: number; flacBytes: number;
      decodeMs: number; encodeMs: number; cpuPct: number};
  };
  log: string[];
};

function parse(json: string): Status | null {
  try {
    return JSON.parse(json) as Status;
  } catch {
    return null;
  }
}

/**
 * Screenatic feasibility probe for Vega OS. Starts the native listeners and shows, live, whether
 * other devices on the LAN reach us. Test steps are printed on screen for Monday's session.
 */
export const App = () => {
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState<string>('');

  useEffect(() => {
    try {
      setStatus(parse(ProbeNative.start('Screenatic Probe')));
      // Stage 0: the real AirPlay core. iPhone can now actually connect (video is counted, not shown).
      console.log(ProbeNative.airplayStart('Screenatic Probe'));
    } catch (e) {
      setError(String(e));
    }
    const timer = setInterval(() => {
      try {
        setStatus(parse(ProbeNative.status()));
      } catch (e) {
        setError(String(e));
      }
    }, 1000);
    return () => {
      clearInterval(timer);
      try {
        ProbeNative.stop();
      } catch {}
    };
  }, []);

  const ip = status?.ip || '?';
  return (
    <View style={styles.root}>
      <View style={styles.left}>
        <Text style={styles.title}>Screenatic Vega Probe</Text>
        <Text style={styles.big}>
          {status?.running ? 'Running' : 'Not running'} · {ip} ({status?.iface || '-'})
        </Text>
        {error ? <Text style={styles.err}>{error}</Text> : null}
        <Section title="Ports">
          {Object.entries(status?.ports ?? {}).map(([k, v]) => (
            <Line key={k} text={`${k}: ${v}`} />
          ))}
          <Line text={`TCP connections accepted: ${status?.tcpAccepted ?? 0}`} />
        </Section>
        <Section title="mDNS (AirPlay discovery)">
          <Line text={status?.mdns.state ?? '-'} />
          <Line
            text={`queries seen ${status?.mdns.queriesSeen ?? 0} · AirPlay queries ${
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
            text={`video: ${status?.airplay?.video.frames ?? 0} frames (${status?.airplay?.video.muxed ?? 0} to MSE, ${
              status?.airplay?.video.inits ?? 0
            } init, ${status?.airplay?.video.keyframes ?? 0} keyframes, last gap ${(
              status?.airplay?.video.keyGapSec ?? -1
            ).toFixed(1)} s), ${Math.round(
              (status?.airplay?.video.bytes ?? 0) / 1024,
            )} KiB, ${status?.airplay?.video.width ?? 0}x${status?.airplay?.video.height ?? 0}`}
          />
          <Line text={`audio: ${status?.airplay?.audio.codec ?? '-'}`} />
          <Line
            text={`audio: ${(status?.airplay?.audio.seconds ?? 0).toFixed(1)} s decoded, ${
              status?.airplay?.audio.decodeErrors ?? 0
            } errors, CPU ${(status?.airplay?.audio.cpuPct ?? 0).toFixed(1)}% (decode ${
              status?.airplay?.audio.decodeMs ?? 0
            } ms + FLAC ${status?.airplay?.audio.encodeMs ?? 0} ms)`}
          />
        </Section>
        <Section title="Video ingest (:7100)">
          <Line
            text={`received ${Math.round((status?.video.received ?? 0) / 1024)} KiB · buffered ${Math.round(
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
          <Line text={`1. From the Mac: curl http://${ip}:7000/  and  curl http://${ip}:49595/description.xml`} />
          <Line text="2. iPhone: Control Center → Screen Mirroring → is “Screenatic Probe” listed?" />
          <Line text="3. Mac: dns-sd -B _airplay._tcp   (should list Screenatic Probe)" />
          <Line text="4. Windows: Win+K / Cast to device, or a DLNA app on Android" />
          <Line text={`5. Video latency: vega/scripts/stream_test.sh ${ip}`} />
        </Section>
      </View>
      <View style={styles.right}>
        <VideoTest />
        <MirrorAudio />
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

const Section = ({title, children}: {title: string; children: React.ReactNode}) => (
  <View style={styles.block}>
    <Text style={styles.section}>{title}</Text>
    {children}
  </View>
);

const Line = ({text}: {text: string}) => <Text style={styles.line}>{text}</Text>;

const styles = StyleSheet.create({
  // Vega lays out in dp at 960x540 on a 1080p screen, so keep everything relative.
  root: {flex: 1, flexDirection: 'row', backgroundColor: '#101418', padding: 20},
  left: {flex: 1, paddingRight: 12},
  right: {flex: 1, backgroundColor: '#182028', padding: 10, borderRadius: 6},
  title: {color: '#fff', fontSize: 22, fontWeight: '600'},
  big: {color: '#9fe39f', fontSize: 15, marginTop: 2, marginBottom: 4},
  err: {color: '#ff8080', fontSize: 13},
  block: {marginTop: 6},
  section: {color: '#7fb2ff', fontSize: 13, fontWeight: '600', marginBottom: 1},
  line: {color: '#e8e8e8', fontSize: 11},
  log: {color: '#c8c8c8', fontSize: 10, fontFamily: 'monospace'},
});
