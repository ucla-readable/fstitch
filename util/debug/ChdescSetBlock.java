import java.io.DataInput;
//import java.io.IOException;

public class ChdescSetBlock extends Opcode
{
	private final int chdesc, block;
	
	public ChdescSetBlock(int chdesc, int block)
	{
		this.chdesc = chdesc;
		this.block = block;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.setBlock(block);
	}
	
	public String toString()
	{
		return "KDB_CHDESC_SET_BLOCK: chdesc = " + SystemState.hex(chdesc) + ", block = " + SystemState.hex(block);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SET_BLOCK, "KDB_CHDESC_SET_BLOCK", ChdescSetBlock.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("block", 4);
		return factory;
	}
}
