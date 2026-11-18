// Replays exact solver samples. No interpolated joint or target positions.
export class Replay {
 constructor(update){this.update=update;this.$=id=>document.getElementById(id);this.key='';this.playing=false;this.current=null;
 this.$('sequence').onchange=()=>{this.stop();update()};
 this.$('frame').oninput=()=>{this.stop();update()};
 this.$('play').onclick=()=>{if(this.playing)this.stop();else if(this.current){if(+this.$('frame').value>=this.current.frames.length-1)this.$('frame').value='0';this.started=performance.now();this.offset=this.current.frames[+this.$('frame').value].time;this.playing=true;this.$('play').textContent='Pause replay'}update()};
 }
 stop(){this.playing=false;this.$('play').textContent='Play replay'}
 read(data,body){const entries=Object.entries(data.sequences||{}).filter(([key])=>key.startsWith(body+':'));const signature=body+':'+entries.map(([k])=>k).join('|');
 if(this.data!==data||this.key!==signature){this.stop();this.data=data;this.key=signature;this.$('sequence').replaceChildren(new Option('Static pose',''));for(const [key,s]of entries)this.$('sequence').append(new Option(s.label,key));this.$('frame').value='0'}
 this.$('replayControls').hidden=!entries.length;this.current=data.sequences?.[this.$('sequence').value]||null;const active=!!this.current;this.$('frame').disabled=!active;this.$('play').disabled=!active;this.$('pose').disabled=active;
 if(!active){this.$('replayStatus').textContent='';return null}
 this.$('frame').max=String(this.current.frames.length-1);const index=Math.min(+this.$('frame').value,this.current.frames.length-1),f=this.current.frames[index];this.$('frame').value=String(index);
 this.$('replayStatus').textContent=`${this.current.description||''}\nFrame ${index+1}/${this.current.frames.length} · ${f.time.toFixed(3)} s · ${f.phase} · left ${f.tracked[0]?'tracked':'LOST'}, right ${f.tracked[1]?'tracked':'LOST'}${f.recovering?'\nRecovery: left '+(f.recovering[0]?'recovering':'settled')+', right '+(f.recovering[1]?'recovering':'settled')+' · loss-fade confidence (not accuracy) '+f.displayConfidence.map(x=>Math.round(x*100)+'%').join(' / '):''}`;return f;
 }
 tick(now){if(!this.playing||!this.current)return;const frames=this.current.frames,time=this.offset+(now-this.started)/1000;let i=+this.$('frame').value;while(i+1<frames.length&&frames[i+1].time<=time)i++;if(i!==+this.$('frame').value){this.$('frame').value=String(i);this.update()}if(i===frames.length-1)this.stop()}
}
