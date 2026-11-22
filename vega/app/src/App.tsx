import React, {useEffect, useState} from 'react';
import {ScrollView, StyleSheet, Text, View} from 'react-native';
import {ProbeNative} from '@amazon-devices/probenative';
import {VideoTest} from './VideoTest';

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
 * ScreenHop feasibility probe for Vega OS. Starts the native listeners and shows, live, whether
 * other devices on the LAN reach us. Test steps are printed on screen for Monday's session.
 */
export const App = () => {
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState<string>('');

  useEffect(() => {
    try {
      setStatus(parse(ProbeNative.start('ScreenHop Probe')));
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
        <Text style={styles.title}>ScreenHop Vega Probe</Text>
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
          <Line text="2. iPhone: Control Center → Screen Mirroring → is “ScreenHop Probe” listed?" />
          <Line text="3. Mac: dns-sd -B _airplay._tcp   (should list ScreenHop Probe)" />
          <Line text="4. Windows: Win+K / Cast to device, or a DLNA app on Android" />
          <Line text={`5. Video latency: vega/scripts/stream_test.sh ${ip}`} />
        </Section>
      </View>
      <View style={styles.right}>
        <VideoTest />
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
  root: {flex: 1, flexDirection: 'row', backgroundColor: '#101418', padding: 40},
  left: {flex: 1, paddingRight: 24},
  right: {width: 800, backgroundColor: '#182028', padding: 16, borderRadius: 8},
  title: {color: '#fff', fontSize: 36, fontWeight: '600'},
  big: {color: '#9fe39f', fontSize: 24, marginTop: 4, marginBottom: 8},
  err: {color: '#ff8080', fontSize: 18},
  block: {marginTop: 10},
  section: {color: '#7fb2ff', fontSize: 20, fontWeight: '600', marginBottom: 2},
  line: {color: '#e8e8e8', fontSize: 17},
  log: {color: '#c8c8c8', fontSize: 14, fontFamily: 'monospace'},
});
