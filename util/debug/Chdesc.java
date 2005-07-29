import java.util.Vector;
import java.util.Iterator;

public class Chdesc
{
	public static final int TYPE_NOOP = 0;
	public static final int TYPE_BIT = 1;
	public static final int TYPE_BYTE = 2;
	public static final int TYPE_DESTROY = 3;
	
	/* these flags must be kept in sync with kfs/chdesc.h */
	public static final int FLAG_MARKED = 0x01;
	public static final int FLAG_INSET = 0x02;
	public static final int FLAG_MOVED = 0x04;
	public static final int FLAG_ROLLBACK = 0x08;
	public static final int FLAG_FREEING = 0x10;
	public static final int FLAG_PRMARKED = 0x20;
	
	public final int address;
	
	private int type, flags;
	private int block, owner;
	
	private short offset; /* for bit, byte */
	private int xor; /* for bit */
	private short length; /* for byte */
	
	private Vector dependencies, dependents;
	
	public Chdesc(int address, int block, int owner)
	{
		this.address = address;
		this.block = block;
		this.owner = owner;
		flags = FLAG_ROLLBACK;
		dependencies = new Vector();
		dependents = new Vector();
		changeToNoop();
	}
	
	public Chdesc(int address, int block, int owner, short offset, int xor)
	{
		this(address, block, owner);
		changeToBit(offset, xor);
	}
	
	public Chdesc(int address, int block, int owner, short offset, short length)
	{
		this(address, block, owner);
		changeToByte(offset, length);
	}
	
	public int getType()
	{
		return type;
	}
	
	public int getBlock()
	{
		return block;
	}
	
	public int getOwner()
	{
		return owner;
	}
	
	public int getFlags()
	{
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
	
	public void setBlock(int block)
	{
		this.block = block;
	}
	
	public void setOwner(int owner)
	{
		this.owner = owner;
	}
	
	public void setFlags(int flags)
	{
		this.flags |= flags;
	}
	
	public void clearFlags(int flags)
	{
		this.flags &= ~flags;
	}
	
	public void changeToNoop()
	{
		type = TYPE_NOOP;
	}
	
	public void changeToBit(short offset, int xor)
	{
		type = TYPE_BIT;
		this.offset = offset;
		this.xor = xor;
	}
	
	public void changeToByte(short offset, short length)
	{
		type = TYPE_BYTE;
		this.offset = offset;
		this.length = length;
	}
	
	public void destroy()
	{
		type = TYPE_DESTROY;
	}
	
	public void addDependency(int dependency)
	{
		//dependencies.add(Integer.valueOf(dependency));
		dependencies.add(new Integer(dependency));
	}
	
	public void addDependent(int dependent)
	{
		//dependents.add(Integer.valueOf(dependent));
		dependents.add(new Integer(dependent));
	}
	
	public Iterator getDependencies()
	{
		return dependencies.iterator();
	}
	
	public Iterator getDependents()
	{
		return dependents.iterator();
	}
}
