import java.io.DataInput;
//import java.io.IOException;

public class ChdescCreateByte extends Opcode
{
	private final int chdesc, block, owner;
	private final short offset, length;
	
	public ChdescCreateByte(int chdesc, int block, int owner, short offset, short length)
	{
		this.chdesc = chdesc;
		this.block = block;
		this.owner = owner;
		this.offset = offset;
		this.length = length;
	}
	
	public void applyTo(SystemState state)
	{
		state.addChdesc(new Chdesc(chdesc, block, owner, offset, length));
	}
	
	public String toString()
	{
		return "KDB_CHDESC_CREATE_BYTE: chdesc = " + SystemState.hex(chdesc) + ", block = " + SystemState.hex(block) + ", owner = " + SystemState.hex(owner) + ", offset = " + offset + ", length = " + length;
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CREATE_BYTE, "KDB_CHDESC_CREATE_BYTE", ChdescCreateByte.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("block", 4);
		factory.addParameter("owner", 4);
		factory.addParameter("offset", 2);
		factory.addParameter("length", 2);
		return factory;
	}
}
