import {pickLanguage} from '../src/i18n';

describe('pickLanguage', () => {
  it('maps system locales to the app languages', () => {
    expect(pickLanguage('en-US')).toBe('en');
    expect(pickLanguage('de_DE')).toBe('de');
    expect(pickLanguage('ja-JP')).toBe('ja');
    expect(pickLanguage('pt-PT')).toBe('pt-BR');
    expect(pickLanguage('zh-CN')).toBe('zh-CN');
    expect(pickLanguage('zh-Hans-CN')).toBe('zh-CN');
    expect(pickLanguage('zh-Hant-TW')).toBe('zh-TW');
    expect(pickLanguage('zh_HK')).toBe('zh-TW');
    expect(pickLanguage('nl-NL')).toBe('en');
    expect(pickLanguage('')).toBe('en');
  });
});
