// Attach source-matched lower garments without replacing the selected upper outfit.
export function withGeneralLowers(base, additions, source, preserveExisting=false) {
  const family=additions.sources[source];
  if(!family)return base;
  const poses={...base.poses}, models={...base.models};
  for(const [key,lower] of Object.entries(family.poses)) {
    if(preserveExisting&&poses[key])continue;
    const [body,style,...poseBits]=key.split(':'), pose=poseBits.join(':');
    const upper=base.poses[`${body}:0:${pose}`]||base.poses[`${body}:${pose}`];
    if(!upper)continue;
    for(const part of lower.parts)models[part.model]=additions.models[part.model];
    poses[key]={...upper,parts:[...upper.parts.filter(p=>!/^av_(wfade|bottom|vector_bottom|nightforge_bottom|tennis_lower|kilt_lower)/.test(p.model)),...lower.parts]};
  }
  return {...base,poses,models};
}
