import java.util.HashSet;
import java.util.Iterator;

public class Chdesc
{
	public static final int TYPE_NOOP = 0;
	public static final int TYPE_BIT = 1;
	public static final int TYPE_BYTE = 2;
	public static final int TYPE_DESTROY = 3;
	public static final int TYPE_DANGLING = 4;
	
	/* these flags must be kept in sync with kfs/chdesc.h */
	public static final int FLAG_MARKED = 0x01;
	public static final int FLAG_ROLLBACK = 0x02;
	public static final int FLAG_WRITTEN = 0x04;
	public static final int FLAG_FREEING = 0x08;
	public static final int FLAG_DATA = 0x10;
	public static final int FLAG_BIT_NOOP = 0x20;
	public static final int FLAG_OVERLAP = 0x40;
	public static final int FLAG_SAFE_AFTER = 0x80;
	public static final int FLAG_INFLIGHT = 0x100;
	public static final int FLAG_DBWAIT = 0x8000;
	
	public final int address, opcode;
	
	private int type, flags;
	private int block, owner;
	private String label;
	
	private short offset; /* for bit, byte */
	private int xor; /* for bit */
	private short length; /* for byte */
	
	private Chdesc free_prev, free_next;
	
	private ChdescCollection befores, afters;
	private HashSet locations;
	
	public Chdesc(int address, int opcode)
	{
		this.address = address;
		this.opcode = opcode;
		type = TYPE_DANGLING;
	}
	
	public Chdesc(int address, int block, int owner, int opcode)
	{
		this.address = address;
		this.block = block;
		this.owner = owner;
		this.opcode = opcode;
		befores = new ChdescCollection();
		afters = new ChdescCollection();
		locations = new HashSet();
		changeToNoop();
	}
	
	public Chdesc(int address, int block, int owner, int opcode, short offset, int xor)
	{
		this(address, block, owner, opcode);
		changeToBit(offset, xor);
		/* BIT chdescs start rolled back */
		setFlags(FLAG_ROLLBACK);
	}
	
	public Chdesc(int address, int block, int owner, int opcode, short offset, short length)
	{
		this(address, block, owner, opcode);
		changeToByte(offset, length);
		/* BYTE chdescs start rolled back */
		setFlags(FLAG_ROLLBACK);
	}
	
	public int getType()
	{
		return type;
	}
	
	public boolean isValid()
	{
		return type == TYPE_NOOP || type == TYPE_BIT || type == TYPE_BYTE;
	}
	
	public int getBlock()
	{
		if(!isValid())
			throw new RuntimeException("Query for block of invalid chdesc!");
		return block;
	}
	
	public int getOwner()
	{
		if(!isValid())
			throw new RuntimeException("Query for owner of invalid chdesc!");
		return owner;
	}
	
	public int getFlags()
	{
		if(!isValid())
			throw new RuntimeException("Query for flags of invalid chdesc!");
		return flags;
	}
	
	public short getOffset()
	{
		if(type != TYPE_BIT && type != TYPE_BYTE)
			throw new RuntimeException("Query for offset of non-BIT/BYTE chdesc!");
		return offset;
	}
	
	public int getXor()
	{
		if(type != TYPE_BIT)
			throw new RuntimeException("Query for xor of non-BIT chdesc!");
		return xor;
	}
	
	public short getLength()
	{
		if(type != TYPE_BYTE)
			throw new RuntimeException("Query for length of non-BYTE chdesc!");
		return length;
	}
	
	public String getLabel()
	{
		return label;
	}
	
	public Chdesc getFreePrev()
	{
		if(!isValid())
			throw new RuntimeException("Query for free_prev of invalid chdesc!");
		return free_prev;
	}
	
	public Chdesc getFreeNext()
	{
		if(!isValid())
			throw new RuntimeException("Query for free_next of invalid chdesc!");
		return free_next;
	}
	
	public void setBlock(int block)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to set block of invalid chdesc!");
		this.block = block;
	}
	
	public void setOwner(int owner)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to set owner of invalid chdesc!");
		this.owner = owner;
	}
	
	public void setOffset(short offset)
	{
		if(type != TYPE_BIT && type != TYPE_BYTE)
			throw new RuntimeException("Attempt to set offset of non-BIT/BYTE chdesc!");
		this.offset = offset;
	}
	
	public void setLength(short length)
	{
		if(type != TYPE_BYTE)
			throw new RuntimeException("Attempt to set offset of non-BYTE chdesc!");
		this.length = length;
	}
	
	public void setFlags(int flags)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to set flags of invalid chdesc!");
		this.flags |= flags;
	}
	
	public void clearFlags(int flags)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to clear flags of invalid chdesc!");
		this.flags &= ~flags;
	}
	
	public void setLabel(String label)
	{
		this.label = label;
	}
	
	public void setFreePrev(Chdesc free_prev)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to set free_prev of invalid chdesc!");
		this.free_prev = free_prev;
	}
	
	public void setFreeNext(Chdesc free_next)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to set free_next of invalid chdesc!");
		this.free_next = free_next;
	}
	
	public void changeToNoop()
	{
		if(!isValid())
			throw new RuntimeException("Attempt to change type of invalid chdesc!");
		type = TYPE_NOOP;
	}
	
	public void changeToBit(short offset, int xor)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to change type of invalid chdesc!");
		type = TYPE_BIT;
		this.offset = offset;
		this.xor = xor;
	}
	
	public void changeToByte(short offset, short length)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to change type of invalid chdesc!");
		type = TYPE_BYTE;
		this.offset = offset;
		this.length = length;
	}
	
	public void destroy()
	{
		if(!isValid())
			throw new RuntimeException("Attempt to destroy invalid chdesc!");
		type = TYPE_DESTROY;
	}
	
	public void addBefore(Chdesc before)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to add before to invalid chdesc!");
		befores.add(before);
	}
	
	public void addAfter(Chdesc after)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to add after to invalid chdesc!");
		afters.add(after);
	}
	
	public void remBefore(int before)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to remove before from invalid chdesc!");
		befores.remove(before);
	}
	
	public void remAfter(int after)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to remove after from invalid chdesc!");
		afters.remove(after);
	}
	
	public Iterator getBefores()
	{
		if(!isValid())
			throw new RuntimeException("Query for befores of invalid chdesc!");
		return befores.iterator();
	}
	
	public Iterator getAfters()
	{
		if(!isValid())
			throw new RuntimeException("Query for afters of invalid chdesc!");
		return afters.iterator();
	}
	
	public int getAfterCount()
	{
		if(!isValid())
			throw new RuntimeException("Query for after count of invalid chdesc!");
		return afters.size();
	}
	
	public void weakRetain(int location)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to weak retain an invalid chdesc!");
		//locations.add(Integer.valueOf(location));
		locations.add(new Integer(location));
	}
	
	public void weakForget(int location)
	{
		if(!isValid())
			throw new RuntimeException("Attempt to weak forget an invalid chdesc!");
		//locations.remove(Integer.valueOf(location));
		locations.remove(new Integer(location));
	}
	
	public Iterator getLocations()
	{
		if(!isValid())
			throw new RuntimeException("Query for weak references to invalid chdesc!");
		return locations.iterator();
	}
	
	public String toString()
	{
		String value = "[chdesc " + SystemState.hex(address) + ": ";
		if(type <= TYPE_BYTE)
			value += "block " + SystemState.hex(block) + ", owner " + SystemState.hex(owner) + ", ";
		switch(type)
		{
			case TYPE_NOOP:
				value += "NOOP";
				break;
			case TYPE_BIT:
				value += "BIT, offset " + offset + ", xor " + SystemState.hex(xor);
				break;
			case TYPE_BYTE:
				value += "BYTE, offset " + offset + ", length " + length;
				break;
			case TYPE_DESTROY:
				value += "DESTROYED";
				break;
			case TYPE_DANGLING:
				value += "DANGLING";
				break;
			default:
				value += "UNKNOWN TYPE";
				break;
		}
		value += "]";
		return value;
	}
	
	public String renderName()
	{
		return "\"ch" + SystemState.hex(address) + "-hc" + SystemState.hex(hashCode()) + "\"";
	}

	public static String getBlockName(int block, int owner, boolean showBlock, boolean showOwner, SystemState state)
	{
		String name = "";
		if(showBlock && block != 0)
		{
			Bdesc bdesc = state.lookupBdesc(block);
			if(bdesc != null)
				name += "#" + bdesc.number + " (" + SystemState.hex(block) + ")";
			else
				name += "on " + SystemState.hex(block);
		}
		if(showOwner && owner != 0)
		{
			String bdName = state.getBdName(owner);
			if(bdName != null)
				name += bdName;
			else
				name += "at " + SystemState.hex(owner);
		}
		return name;
	}
	
	private String renderBlockOwner(boolean showBlock, boolean showOwner, SystemState state)
	{
		String bName = getBlockName(block, 0, showBlock, false, state);
		if(bName.length() > 0)
			bName = "\\n" + bName;
		String oName = getBlockName(0, owner, false, showOwner, state);
		if(oName.length() > 0)
			oName = "\\n" + oName;
		return bName + oName;
	}
	
	public String render(boolean renderFree, boolean renderBlock, boolean renderOwner, SystemState state)
	{
		String name = renderName();
		
		String links = name + " [label=\"" + SystemState.hex(address);
		if(label != null)
			links += "\\n\\\"" + label + "\\\"";
		switch(type)
		{
			case TYPE_NOOP:
				links += renderBlockOwner(renderBlock, renderOwner, state) + "\",style=\"";
				break;
			case TYPE_BIT:
				links += "\\n[" + offset + ":" + SystemState.hex(xor) + "]" + renderBlockOwner(renderBlock, renderOwner, state) + "\",fillcolor=springgreen1,style=\"filled";
				break;
			case TYPE_BYTE:
				links += "\\n[" + offset + ":" + length + "]" + renderBlockOwner(renderBlock, renderOwner, state) + "\",fillcolor=slateblue1,style=\"filled";
				break;
			case TYPE_DESTROY:
				links += "\",fillcolor=orange,style=\"filled";
				break;
			case TYPE_DANGLING:
				links += "\",fillcolor=red,style=\"filled";
				break;
		}
		if((flags & FLAG_ROLLBACK) != 0)
			links += ",dashed,bold";
		if((flags & FLAG_MARKED) != 0)
			links += ",bold\",color=red";
		else
			links += "\"";
		if((flags & FLAG_FREEING) != 0)
			links += ",fontcolor=red";
		else if((flags & FLAG_WRITTEN) != 0)
			links += ",fontcolor=blue";
		links += "]\n";
		
		Iterator i = befores.iterator();
		while(i.hasNext())
		{
			Chdesc chdesc = (Chdesc) i.next();
			/* we say you are a before for us: black arrows */
			links += name + " -> " + chdesc.renderName() + " [color=black]\n";
		}
		i = afters.iterator();
		while(i.hasNext())
		{
			Chdesc chdesc = (Chdesc) i.next();
			/* we say you are an after for us: gray arrows */
			links += chdesc.renderName() + " -> " + name + " [color=gray]\n";
		}
		i = locations.iterator();
		while(i.hasNext())
		{
			Integer address = (Integer) i.next();
			/* weak references: yellow boxes, green arrows */
			String location = "\"" + SystemState.hex(address.intValue()) + "\"";
			links += location + " [shape=box,fillcolor=yellow,style=filled]\n";
			links += location + " -> " + name + " [color=green]\n";
		}
		if(free_prev != null)
			/* we say you are prev: orange arrows */
			links += free_prev.renderName() + " -> " + name + " [color=orange]\n";
		if(free_next != null && renderFree)
			/* we say you are next: red arrows */
			links += name + " -> " + free_next.renderName() + " [color=red]\n";
		
		return links;
	}
	
	public String renderFlags()
	{
		/* use getFlags() for the type testing */
		return renderFlags(getFlags());
	}
	
	public static String renderFlags(int flags)
	{
		String names = "";
		if((flags & FLAG_MARKED) != 0)
			names += " | MARKED";
		if((flags & FLAG_ROLLBACK) != 0)
			names += " | ROLLBACK";
		if((flags & FLAG_WRITTEN) != 0)
			names += " | WRITTEN";
		if((flags & FLAG_FREEING) != 0)
			names += " | FREEING";
		if((flags & FLAG_DATA) != 0)
			names += " | DATA";
		if((flags & FLAG_BIT_NOOP) != 0)
			names += " | BIT_NOOP";
		if((flags & FLAG_OVERLAP) != 0)
			names += " | FLAG_OVERLAP";
		if((flags & FLAG_DBWAIT) != 0)
			names += " | DBWAIT";
		if((flags & FLAG_SAFE_AFTER) != 0)
			names += " | SAFE_AFTER";
		if((flags & FLAG_INFLIGHT) != 0)
			names += " | INFLIGHT";
		names += " = " + SystemState.hex(flags);
		return names.substring(3);
	}
}
