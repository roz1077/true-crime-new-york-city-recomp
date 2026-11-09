# Writes src/dev/level_names.h for the dev debug menu: run from the repository root.
import struct,zlib,re
d=open(r'assets\data\Lang\Eng\LangTable.dat','rb').read(); n=struct.unpack_from('<I',d,0)[0]; t={}
for i in range(4,4+n,8):
    h,o=struct.unpack_from('<II',d,i); p=4+n+o
    if p<len(d): t[h]=d[p:d.index(b'\0',p)].decode('latin1')
names=[l.split(' ',1)[1].strip() for l in open(r'assets\data\AssetlistsDev\LevelInfo.def')]
def words(s): return re.sub(r'(?<=[a-z])(?=[A-Z0-9])|(?<=[0-9])(?=[A-Z])','  ',s).replace('  ',' ')
def pretty(nm):
    m=re.match(r'BC(\d)_M(\d)F(_PROLOGUE)?$',nm,re.I)
    if m: return 'Case %s: %s'%(m.group(1),'chapter intro' if m.group(2)=='0' else 'after mission %s'%m.group(2))+(' (prologue)' if m.group(3) else '')
    m=re.match(r'BC(\d)_Prologue$',nm)
    if m: return 'Case %s prologue'%m.group(1)
    m=re.match(r'FC_M(\d+)_(\w+)$',nm)
    if m: return 'Fight Club %d: %s'%(int(m.group(1)),words(m.group(2)))
    m=re.match(r'StreetRace(\d+)$',nm)
    if m: return 'Street Race %d'%int(m.group(1))
    m=re.match(r'(Combat|Driving|Shooting)_Rank(\d+)$',nm)
    if m: return '%s Rank %d'%(m.group(1),int(m.group(2)))
    m=re.match(r'(Madam|King|Cabbie)(\w+)$',nm)
    if m and nm not in ('CI_King',): return '%s: %s'%(m.group(1),words(m.group(2)))
    m=re.match(r'(End[GB])_M1_(Prologue|Epilogue)$',nm)
    if m: return '%s ending %s'%('Good cop' if m.group(1)=='EndG' else 'Bad cop',m.group(2).lower())
    m=re.match(r'INT_M(\d)(F?)_?(.*)$',nm,re.I)
    if m: return 'Intro %s%s%s'%(m.group(1),' follow-up' if m.group(2) else '',(' '+m.group(3).capitalize()) if m.group(3) else '')
    if nm.startswith('TEST'): return 'Test: '+nm[4:].lstrip('_').replace('_',' ').capitalize()
    fixed={'CICabbieMission':'Informant: Cabbie','CIKingMission':'Informant: King','CIMadamMission':'Informant: Madam',
           'FightClub':'Fight Club','FightClubBar':'Fight Club bar','StreetRacing':'Street Racing','StreetRaceQualify':'Street Race qualifier',
           'PoliceChallenges':'Police Challenges','EndGame':'Ending','End_FreeRoam':'Ending free roam','INTRO':'Intro',
           'ATTRACTMODEMS':'Attract mode','GRANDCENTRALSTATION':'Grand Central Station','FIGHTARENA':'Fight Arena','WEAPONARENA':'Weapon Arena',
           'POLICEPRECINCTDEMOTION':'Police precinct demotion','SC03':'SC03','SC03_M1':'SC03 mission 1'}
    return fixed.get(nm)
q=lambda s:'"'+s.replace(chr(92),chr(92)*2).replace('"',chr(92)+'"')+'"'
out=['// Generated from assets/data/AssetlistsDev/LevelInfo.def (every case and mission name in the story',
     '// table, LevelInfo.tag) and assets/data/Lang/Eng/LangTable.dat (the game\'s English titles, keyed by the',
     '// same names). "official" titles are the game\'s own; the rest are made readable from the name.',
     '#pragma once','','namespace dev {','struct LevelName {','    const char *name, *title;','    bool official;','};',
     'inline constexpr LevelName kLevelInfoNames[] = {']
for nm in sorted(names):
    h=zlib.crc32(nm.encode())
    if h in t: out.append('    {%s, %s, true},'%(q(nm),q(t[h])))
    else:
        p=pretty(nm); out.append('    {%s, %s, false},'%(q(nm),q(p) if p else 'nullptr'))
out+=['};','} // namespace dev','']
open(r'src\dev\level_names.h','w',newline='\n').write('\n'.join(out))
