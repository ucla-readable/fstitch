public class ChdescConvertByte extends Opcode
{
	private final int chdesc;
	private final short offset, length;
	
	public ChdescConvertByte(int chdesc, short offset, short length)
	{
		this.chdesc = chdesc;
		this.offset = offset;
		this.length = length;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.changeToByte(offset, length);
	}
	
	public String toString()
	{
		return "KDB_CHDESC_CONVERT_BYTE: chdesc = " + SystemState.hex(chdesc) + ", offset = " + offset + ", length = " + length;
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CONVERT_BYTE, "KDB_CHDESC_CONVERT_BYTE", ChdescConvertByte.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("offset", 2);
		factory.addParameter("length", 2);
		return factory;
	}
}
