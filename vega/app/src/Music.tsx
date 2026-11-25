import React, {useEffect, useRef, useState} from 'react';
import {onAirPlayEvent} from './airplay';
import {NowPlaying} from './NowPlaying';

type Track = {title?: string; artist?: string; album?: string};

/**
 * Audio-only AirPlay (Apple Music, Podcasts...): the sound plays in the native player; this shows
 * the track, cover and progress the sender reports, until it disconnects or mirroring starts.
 */
export const Music = ({onActive}: {onActive: (active: boolean) => void}) => {
  const [active, setActive] = useState(false);
  const [client, setClient] = useState('');
  const [track, setTrack] = useState<Track>({});
  const [cover, setCover] = useState('');
  const [paused, setPaused] = useState(false);
  const [duration, setDuration] = useState(0);
  const [position, setPosition] = useState(0);
  // The sender reports progress only now and then: count on from the last report while playing.
  const anchor = useRef({pos: 0, at: 0, playing: false});
  const onActiveRef = useRef(onActive);
  onActiveRef.current = onActive;

  useEffect(() => {
    const show = (on: boolean) => {
      setActive(on);
      onActiveRef.current(on);
    };
    const unsubscribe = onAirPlayEvent((ev) => {
      if (ev.type === 'music') {
        if (ev.state === 'connected') {
          setClient(ev.client ?? '');
          setTrack({});
          setCover('');
          setDuration(0);
          setPosition(0);
          setPaused(false);
          anchor.current = {pos: 0, at: Date.now(), playing: true};
          show(true);
        } else {
          const playing = ev.state === 'playing';
          setPaused(!playing);
          const a = anchor.current;
          const now = Date.now();
          if (a.playing) a.pos += (now - a.at) / 1000;
          a.at = now;
          a.playing = playing;
        }
      } else if (ev.type === 'track') {
        setTrack({title: ev.title, artist: ev.artist, album: ev.album});
      } else if (ev.type === 'cover') {
        setCover(ev.uri);
      } else if (ev.type === 'progress') {
        setDuration(ev.duration);
        anchor.current = {
          pos: ev.position,
          at: Date.now(),
          playing: anchor.current.playing,
        };
        setPosition(ev.position);
      } else if (
        ev.type === 'disconnected' ||
        ev.type === 'mirror' ||
        ev.type === 'play'
      ) {
        show(false);
      }
    });
    const tick = setInterval(() => {
      const a = anchor.current;
      if (a.playing) setPosition(a.pos + (Date.now() - a.at) / 1000);
    }, 1000);
    return () => {
      unsubscribe();
      clearInterval(tick);
    };
  }, []);

  if (!active) return null;
  return (
    <NowPlaying
      {...track}
      artworkUri={cover || undefined}
      client={client}
      position={Math.min(position, duration || position)}
      duration={duration}
      paused={paused}
    />
  );
};
