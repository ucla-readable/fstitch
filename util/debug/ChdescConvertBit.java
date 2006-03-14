import java.io.DataInput;
//import java.io.IOException;

public class ChdescConvertBit extends Opcode
{
	private final int chdesc, xor;
	private final short offset;
	
	public ChdescConvertBit(int chdesc, short offset, int xor)
	{
		this.chdesc = chdesc;
		this.offset = offset;
		this.xor = xor;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.changeToBit(offset, xor);
	}
	
	public String toString()
	{
		return "KDB_CHDESC_CONVERT_BIT: chdesc = " + SystemState.hex(chdesc) + ", offset = " + offset + ", xor = " + SystemState.hex(xor);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CONVERT_BIT, "KDB_CHDESC_CONVERT_BIT", ChdescConvertBit.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("offset", 2);
		factory.addParameter("xor", 4);
		return factory;
	}
}
