"""Build both Peluru costumes for both avatar bodies from the open neutral rig."""
from pathlib import Path
import math
import struct
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
import numpy as np
from PIL import Image, ImageDraw
import preview_render as reader
from build_open_assets import mdl_bytes, write_model

ASSETS = Path(__file__).resolve().parents[1] / 'app/assets_open'
INK, SHADOW, GOLD, LIGHT = '#23202d', '#130f20', '#e9a734', '#ffde88'
PINK, RED, JADE, LEAF = '#c02668', '#aa263c', '#11777c', '#319882'
IVORY, SKIN = '#f2dfb9', '#b98366'
TAU = math.tau


def flower(d, x, y, r, color=PINK):
    for ring, scale, petals, fill in [(0, 1, 7, RED), (.35, .76, 6, color), (.7, .43, 5, GOLD)]:
        for j in range(petals):
            t = TAU*j/petals + ring
            a, b = x+r*scale*.58*math.cos(t), y+r*scale*.58*math.sin(t)
            k = r*scale*.45
            d.ellipse((a-k, b-k, a+k, b+k), fill)
    k = max(1, r*.18)
    d.ellipse((x-k,y-k,x+k,y+k), LIGHT)


def sprig(d, x, y, r):
    for sign in (-1,1):
        pts = [(x+sign*r*t, y+r*(t*t-.7*t)) for t in np.linspace(0,1.7,20)]
        d.line(pts, fill=GOLD, width=max(1,int(r*.10)))
        for t in (.45,.85,1.25):
            a, b = x+sign*r*t, y+r*(t*t-.7*t)
            d.polygon([(a,b),(a+sign*r*.35,b-r*.62),(a+sign*r*.32,b-r*.1)],fill=LEAF)
    flower(d,x,y,r)


def atlas(dress):
    # Panels: jacket front and back, painted face, then 16 repeatable woven trims.
    im = Image.new('RGB',(1024,1024),INK)
    for back in (False,True):
        panel = Image.new('RGB',(512,512), INK)
        d = ImageDraw.Draw(panel)
        for x in range(0,512,5):
            d.line((x,0,x+70,512),fill='#2c2833')
        if not back and not dress:
            d.polygon([(204,0),(305,0),(256,285)],fill=IVORY)
            for sign in (-1,1):
                d.polygon([(256+sign*57,15),(256+sign*91,61),(256+sign*62,91),(256+sign*76,99),(256,301)],fill='#48404b')
                d.line([(256+sign*65,32),(256+sign*78,64),(256+sign*52,96),(256,301)],fill=GOLD,width=3)
                d.polygon([(256,65),(256+sign*37,48),(256+sign*32,82)],fill=SHADOW)
            for y in range(100,210,26):
                d.ellipse((253,y,259,y+6),fill=INK)
        if dress:
            d.polygon([(151,54),(256,95),(361,54),(334,103),(256,144),(178,103)],fill=JADE)
            d.line([(150,62),(256,111),(362,62)],fill=GOLD,width=7)
            d.rectangle((110,362,402,423),fill=PINK)
            d.line((110,375,402,375), fill=GOLD,width=5)
        else:
            d.rectangle((105,392,407,420),fill='#56362c')
            d.rounded_rectangle((235,390,278,422),6,fill=GOLD,outline=LIGHT,width=3)
        for x in (136,376):
            for y in (116,182,247,311,455):
                sprig(d,x,y,10)
        # Gold barrel beads on a stitched leather sash, continuous across the back.
        start, end = ((151,39),(363,405)) if back else ((361,39),(149,405))
        d.line([start,end],fill=GOLD,width=40)
        d.line([start,end],fill='#543527',width=35)
        for t in np.linspace(.025,.975,19):
            x,y=np.array(start)*(1-t)+np.array(end)*t
            sign=1 if back else -1
            d.line((x-10,y+6*sign,x+10,y-6*sign),fill='#9d6329',width=12)
            d.line((x-10,y+6*sign-2,x+10,y-6*sign-2),fill=GOLD,width=8)
            d.line((x-9,y+6*sign-4,x+9,y-6*sign-4),fill=LIGHT,width=3)
        sprig(d,345 if not back else 254,100 if not back else 322,18)
        im.paste(panel,(512*back,0))
    face = Image.new('RGB',(512,512),'#f0ddc4')
    d = ImageDraw.Draw(face)
    for x in (175,337):
        for j in range(14):
            t = TAU*j/14
            a,b=x+56*math.cos(t),236+45*math.sin(t)
            d.ellipse((a-9,b-9,a+9,b+9),fill=RED)
            d.ellipse((a-3,b-3,a+3,b+3),fill=GOLD)
        d.ellipse((x-52,196,x+52,276),fill=SHADOW)
        sprig(d,x,301,9)
        d.ellipse((x-21,226,x+21,245),fill='#ebd7b8')
        d.ellipse((x-9,225,x+9,246),fill='#49312f')
        d.ellipse((x-4,228,x+4,242),fill=SHADOW)
        d.ellipse((x+1,229,x+5,233),fill=IVORY)
    d.polygon([(256,270),(232,315),(249,321),(256,309),(266,321),(281,315)],fill=SHADOW)
    d.arc((157,298,355,373),0,180,fill=SHADOW,width=9)
    for x in range(183,333,13):
        y=367-.0038*(x-256)**2
        d.line((x-3,y-12,x+3,y+13),fill=SHADOW,width=4)
    sprig(d,256,143,17)
    im.paste(face,(0,512))
    trim_colors=[INK,JADE,PINK,IVORY,GOLD,RED,SKIN,SHADOW,LEAF,'#592542','#47312c',LIGHT,'#d85637','#34323c','#e4b98e','#83334e']
    for k,col in enumerate(trim_colors):
        tile=Image.new('RGB',(128,128),col)
        d=ImageDraw.Draw(tile)
        if k in (0,1,2,3,13):
            for y in (7,118):
                d.line((0,y,128,y),fill=GOLD,width=4)
                for x in range(-8,129,16):
                    d.line([(x,y),(x+8,y+7),(x+16,y)],fill=LIGHT,width=2)
            sprig(d,64,83 if k==1 else 60,16 if k==1 else 22)
            if k==1:
                d.arc((18,39,73,92),10,163,fill=GOLD,width=2)
                d.arc((59,39,114,92),20,166,fill=GOLD,width=2)
                flower(d,64,39,6,GOLD)
            for x in (12,114):
                flower(d,x,83,9,GOLD)
        if k==13:
            tile=Image.new('RGB',(128,128),'#41404a')
            d=ImageDraw.Draw(tile)
            d.line((120,0,120,128),fill=GOLD,width=4)
            d.line((114,0,114,128),fill=LEAF,width=2)
        if k==3:
            tile=Image.new('RGB',(128,128),'#edc98b')
            d=ImageDraw.Draw(tile)
            for x in range(-16,144,16):
                d.ellipse((x,34,x+13,103),outline='#a4754a',width=2)
                d.line([(x,25),(x+8,7),(x+16,25)],fill=IVORY,width=3)
        if k==14:
            tile=Image.new('RGB',(128,128),JADE)
            d=ImageDraw.Draw(tile)
            for y in (45,83):
                sprig(d,62,y,11 if y<80 else 17)
            for x in range(-16,140,16):
                d.arc((x,106,x+16,121),0,180,fill=LIGHT,width=3)
            d.line((0,123,128,123),fill=GOLD,width=3)
        if k==15:
            tile=Image.new('RGB',(128,128),INK)
            d=ImageDraw.Draw(tile)
            d.line((58,0,58,128),fill=GOLD,width=3)
            for y in (24,64,104):
                sprig(d,64,y,9)
        im.paste(tile,(512+k%4*128,512+k//4*128))
    return im


class Costume:
    def __init__(self, sex, dress):
        self.sex, self.dress = sex,dress
        self.verts, self.indices = [],[]
        self.offset=0

    def add(self,v,idx):
        self.verts.append(v)
        self.indices.append(np.asarray(idx,dtype=np.uint32).ravel()+self.offset)
        self.offset+=len(v)

    @staticmethod
    def tile(k,u,v):
        return ((512+128*(k%4)+2+124*u)/1024,(512+128*(k//4)+2+124*v)/1024)

    def surface(self, name, head):
        v,idx,sections,mats=reader.load_mdl(name)
        self.dtype=v.dtype
        for start,count,material in sections:
            tris=idx[start:start+count].reshape(-1,3)
            channel=(mats[material]['flags']>>8)&15
            for back in (False,True):
                pos=v['p'][tris].mean(axis=1)
                group=tris[(pos[:,2]>(-.07 if head else -.033))==back]
                if not len(group):
                    continue
                points=v['p'][group].mean(axis=1)
                x,y,z=points.T
                region=np.zeros(len(group),int)
                if not head:
                    region[abs(x)>.204]=1
                    if self.dress: region[abs(x)>.30]=2
                    region[(y>.018)&(abs(x)<.083)]=2
                for zone in np.unique(region):
                    ids,inv=np.unique(group[region==zone],return_inverse=True)
                    part=v[ids].copy()
                    x,y,z=part['p'].T
                    if head:
                        uv=np.stack([np.clip(.5+x/.20,.004,.996)*.5,.5+.5*np.clip((.293-y-(.008 if self.sex=='f' else 0))/.252,.004,.996)],axis=1)
                        if back or channel==6:
                            uv[:]=self.tile(7,.5,.5)
                    elif zone:
                        uv=np.array(self.tile(6 if zone==2 else 0,np.clip((abs(x)-.20)/.32,0,1),np.clip((.04-y)/.38,0,1))).T
                    else:
                        u=np.clip(.5+(x if not back else -x)/.56,.004,.996)
                        uv=np.stack([.5*(u+back),.5*np.clip((.035-y)/.50,.004,.996)],axis=1)
                    part['u0']=uv
                    self.add(part,inv)

    def loft(self, rows, tile, bone=0, slices=36, repeats=6, center=(0,0), wave=0, lift=0, hem=False):
        # Each ring is (height, half-width, half-depth). Front is negative Z.
        xyz=[];uv=[];idx=[]
        for j,(y,rx,rz) in enumerate(rows):
            v=j/(len(rows)-1)
            for i in range(slices):
                for k in (i,i+1):
                    t=TAU*k/slices
                    fl=1+wave*v*math.cos(12*t)
                    brim_curl = max(0, rx-.10)*(.25*math.sin(t)**2 + .10*math.cos(t)) if bone==2 else 0
                    xyz.append((center[0]+rx*fl*math.sin(t),y+brim_curl+lift*v*(1+math.cos(t))/2,center[1]-rz*fl*math.cos(t)))
                    u=(i*repeats%slices+(k-i)*repeats)/slices
                    part_tile = (15 if abs(math.sin(t))>.91 else 7) if tile=='trousers' else tile
                    uv.append(self.tile(part_tile,u,v if not hem else ((.02,.08,.83,.98)[j] if len(rows)==4 else (.02,.70,.98)[j])))
                if j:
                    a=(j-1)*2*slices+i*2;b=a+2*slices
                    idx.extend([a,a+1,b,b,a+1,b+1])
        p=np.array(xyz);tri=np.array(idx).reshape(-1,3)
        n=np.zeros_like(p)
        normals=np.cross(p[tri[:,1]]-p[tri[:,0]],p[tri[:,2]]-p[tri[:,0]])
        for col in tri.T: np.add.at(n,col,normals)
        # Outward normals for horizontal and sloped profile sections.
        vtx=np.zeros(len(p),self.dtype)
        vtx['p']=p;vtx['n']=n/np.maximum(np.linalg.norm(n,axis=1)[:,None],1e-10)
        vtx['u0']=uv;vtx['bi'][:,0]=bone;vtx['bw'][:,0]=255;vtx['c']=255
        self.add(vtx,idx)

    def fabric(self, point, tile, columns, rows, bone=0, repeat=1):
        """Sample one cloth pattern piece; split only where the woven motif repeats."""
        for panel in range(repeat):
            uvs = [(u / columns, v / rows) for v in range(rows + 1)
                   for u in range(columns + 1)]
            points = np.array([point((panel + u) / repeat, v) for u, v in uvs])
            faces = []
            for j in range(rows):
                for i in range(columns):
                    a = j * (columns + 1) + i
                    b = a + columns + 1
                    faces.extend([(a, a+1, b), (b, a+1, b+1)])
            faces = np.array(faces)
            normals = np.zeros_like(points)
            cross = np.cross(points[faces[:,1]] - points[faces[:,0]],
                             points[faces[:,2]] - points[faces[:,0]])
            for corner in faces.T:
                np.add.at(normals, corner, cross)
            mesh = np.zeros(len(points), self.dtype)
            mesh['p'] = points
            mesh['n'] = normals / np.maximum(np.linalg.norm(normals,axis=1)[:,None], 1e-10)
            mesh['u0'] = [self.tile(tile, u, v) for u, v in uvs]
            mesh['bi'][:,0] = bone
            mesh['bw'][:,0] = 255
            mesh['c'] = 255
            self.add(mesh, faces)

    def bloom(self,x,y,z,r,bone,color):
        # Overlapping cupped petals spiral around an upright, forward-facing corolla.
        for ring, count, radius, shade in [(0,8,1,color),(1,6,.70,color),(2,4,.44,4)]:
            for petal in range(count):
                angle = TAU * petal / count + ring * .54
                def petal_point(u,v):
                    spread = (u-.5) * (1.1 - .72*v)
                    rad = r * radius * (1-.65*v) * (1-.21*(2*u-1)**2)
                    a = angle + spread + .30*v
                    return (x+rad*math.cos(a), y+rad*math.sin(a),
                            z-r*(.16*ring+.25*math.sin(math.pi*u)+.18*v))
                self.fabric(petal_point,shade,4,2,bone)
        for sign in (-1,1):
            def leaf(u,v):
                return (x+sign*r*(.3+1.2*v),y+r*(.34+.81*v+(u-.5)*.65*math.sin(math.pi*v)),
                        z+.004-r*.17*math.sin(math.pi*v))
            self.fabric(leaf,8,2,4,bone)

    def dress_panels(self):
        # The waist gathers at one hip. The lifted front exposes the boots and the train hangs behind.
        for layer, (length,width,depth,tile) in enumerate([
                (.83,.33,.30,2),(.73,.34,.29,9),(.62,.35,.285,2),(.52,.36,.28,14)]):
            def drape(u,v,lace=False):
                t = TAU*u
                lift = ((1+math.cos(t-.43))/2)**1.85
                span = length - (.40 if layer<3 else .29)*lift
                hem = -.39-span
                # Use the same fold phase at the same physical height on all tiers.
                # Radial spacing keeps the outer skirt in front throughout the sweep.
                fall = span*v
                fullness = math.sin(min(1,fall/.83)*math.pi/2)
                fold = fullness*(.012*math.cos(15*t+2.4*fall)+.017*math.sin(8*t-2*fall))
                radius = .146+(.195+layer*.010)*fullness+layer*.006+fold
                rz = .116+(.16+layer*.010)*fullness+layer*.006+fold
                y = -.39-fall + .009*fullness*math.cos(15*t+2.4*fall)
                if lace:
                    y -= .017*(.55+.45*math.cos(36*t))
                    radius += .003
                    rz += .003
                return (radius*math.sin(t)+.038*fall*fall,y,-.036-rz*math.cos(t)+.07*fall*fall)
            self.fabric(drape,tile,8,5,repeat=12)
            if layer in (2,3):
                def edge(u,v):
                    a=np.array(drape(u,.983))
                    b=np.array(drape(u,1,True))
                    return a+(b-a)*v
                self.fabric(edge,3,10,2,repeat=12)
        def sash(u,v):
            t = TAU*u
            squeeze=(1+math.sin(t+.2))/2
            pleat=.007*math.cos(v*5*math.pi+2*math.sin(t))
            return ((.167+pleat)*math.sin(t),-.325-.075*v+.026*math.sin(t)+.019*v*squeeze,
                    -.042-(.136+pleat)*math.cos(t))
        self.fabric(sash,9,8,10,repeat=8)
        for sign in (-1,1):
            def bow(u,v):
                swell=math.sin(math.pi*v)
                return (.11+sign*(.009+.081*v),-.362+.035*v+(u-.5)*(.013+.079*swell),
                        -.153-.026*swell+.009*math.cos(TAU*u))
            self.fabric(bow,2,5,7)
            def ribbon(u,v):
                return (.112+.074*sign*v+(u-.5)*.043,-.372-.224*v+.014*u*v,
                        -.155-.074*v+.014*math.sin(2*TAU*v+u))
            self.fabric(ribbon,2,4,9)
        self.bloom(.11,-.359,-.173,.034,0,12)

    def tailoring(self):
        vertices,indices,_,_=reader.load_mdl('av_top_0_'+self.sex)
        triangles=vertices['p'][indices.reshape(-1,3)].astype(float)
        def front(x,y):
            a,b,c=triangles.transpose(1,0,2)
            ba,ca=b-a,c-a
            dx,dy=x-a[:,0],y-a[:,1]
            det=ba[:,0]*ca[:,1]-ba[:,1]*ca[:,0]
            det=np.where(abs(det)>1e-10,det,np.inf)
            u=(dx*ca[:,1]-dy*ca[:,0])/det
            v=(dy*ba[:,0]-dx*ba[:,1])/det
            keep=(u>=0)&(v>=0)&(u+v<=1)&np.isfinite(det)
            return np.min((a[:,2]+u*ba[:,2]+v*ca[:,2])[keep]) if keep.any() else -.11
        for side in (-1,1):
            def lapel(u,v):
                # A peaked notch and rolled inner edge follow the chest surface.
                y=.019-.279*v
                inner=.047*(1-v)+.003
                width=np.interp(v,[0,.21,.27,.33,.52,1],[.017,.062,.032,.058,.039,.002])
                x=side*(inner+width*u)
                return (x,y,front(x,y)-.006-.009*math.sin(math.pi*u))
            self.fabric(lapel,13,5,20)
            def tail(u,v):
                t=side*(.26+u*(math.pi-.31))
                radius=.148+.044*v
                end=-.453-.068*(1-math.cos(t))-.025*math.sin(t)**2
                return (radius*math.sin(t),-.332+(end+.332)*v,
                        -.040-(.128+.049*v)*math.cos(t))
            self.fabric(tail,0,18,6)
        self.bloom(-.088,-.089,front(-.088,-.089)-.019,.026,0,12)

    def build(self):
        self.surface('av_top_0_'+self.sex,False)
        self.surface('av_bhead_'+self.sex,True)
        # Embroidered brim above the eyes, crown and raised flower cluster follow the head.
        z=-.029; h=.246 if self.sex=='m' else .241
        self.loft([(h+.127,.008,.008),(h+.119,.067,.062),(h+.086,.078,.075),(h+.035,.094,.088),(h+.012,.103,.096),(h+.018,.17,.15),(h+.044,.229,.192),(h+.035,.226,.19),(h+.007,.17,.15),(h+.002,.095,.09)],7,2,48,8,(0,z))
        self.loft([(h+.045,.230,.193),(h+.035,.23,.193),(h+.029,.222,.187)],0,2,48,12,(0,z))
        self.loft([(h+.026,.218,.183),(h+.001,.161,.145)],0,2,48,12,(0,z))
        self.loft([(h+.045,.093,.087),(h+.021,.102,.096)],2,2,36,6,(0,z))
        self.loft([(h+.027,.083,.082),(h-.018,.085,.091),(h-.063,.080,.085)],7,2,32,4,(0,.014))
        for x,y,zz,r,k in [(-.062,.050,-.065,.029,2),(-.018,.052,-.084,.027,12),(.026,.042,-.088,.023,5),(-.09,.037,-.04,.018,4)]:
            self.bloom(x,h+y+.043,z+zz-.014,r*1.42,2,k)
        waist=-.405; center=(0,-.039)
        if self.dress:
            self.dress_panels()
            for sign in (-1,1):
                shoulder=.205 if self.sex=='m' else .169
                for drop,r,k in [(0,.080,3),(.019,.086,0)]:
                    self.loft([(-.045+drop,.046,.055),(-.087+drop,r,.084),(-.103+drop,r*1.03,.088)],k,3 if sign<0 else 4,36,6,(sign*shoulder,.008),wave=.06)
        else:
            self.tailoring()
        for s in (-1,1):
            leg=(s*.088,-.037)
            self.loft([(-.434,.091,.107),(-.57,.092,.100),(-.81,.070,.078),(-.98,.061,.067),(-1.19,.049,.052)],6 if self.dress else 'trousers',slices=24,repeats=3,center=leg)
            self.loft([(-1.008 if self.dress else -1.15,.057,.06),(-1.175,.050,.054),(-1.206,.051,.064)],13,slices=24,repeats=3,center=leg)
            def vamp(u,v):
                angle=TAU*u
                profile=math.sin(math.pi*v/2)
                length=.061+.067*profile
                tip=(1+math.cos(angle))/2
                x=leg[0]+(.050+.009*profile)*(1-.29*tip)*math.sin(angle)
                return (x,-1.177-.073*v-.014*tip,leg[1]-.040*profile-length*math.cos(angle))
            self.fabric(vamp,0,6,5,repeat=4)
            self.loft([(-1.237,.041,.039),(-1.271,.036,.032)],10,slices=16,repeats=2,center=(leg[0],-.004))
        v=np.concatenate(self.verts);i=np.concatenate(self.indices).astype('<u4')
        assert np.all(v['bw'].sum(1)==255) and i.max()<len(v) and np.all(np.isfinite(v['p']))
        name='av_top_%d_%s'%((17 if self.dress else 16),self.sex)
        tex='peluru_'+('vestido' if self.dress else 'traje')
        write_model(str(ASSETS/(name+'.mdl')),mdl_bytes(3,v,i,[(0,len(i),0)],[(0,(1,1,1,1),(0,0,0,0),0,1,tex)]))
        print(name,len(i)//3,'triangles, one material')


if __name__=='__main__':
    reader.ASSETS=str(ASSETS)
    (ASSETS/'tex').mkdir(exist_ok=True)
    for dress,name in [(False,'traje'),(True,'vestido')]:
        im=atlas(dress).convert('RGBA')
        (ASSETS/'tex'/('peluru_'+name+'.tex')).write_bytes(b'STEX'+struct.pack('<3I',*im.size,0)+im.tobytes())
        for sex in 'mf': Costume(sex,dress).build()
